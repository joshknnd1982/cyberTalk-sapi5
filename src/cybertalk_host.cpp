// CyberTalkHost.exe - owns the CyberTalk engine process and serves speech to
// the x86 / x64 SAPI 5 DLLs and the configuration utility over a named pipe.
//
// One host per user session.  It is launched on demand by the first client,
// keeps the engine warm, restarts it if it dies, and exits after ten minutes
// without any connected client (or on CMD_SHUTDOWN).
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <cstring>
#include <cmath>

#include "ct_log.h"
#include "pipe_protocol.h"
#include "stl_engine.h"

extern "C" {
#include "sonic.h"
}

namespace {

constexpr DWORD IDLE_EXIT_MS = 10 * 60 * 1000;

stl::Engine g_engine;
std::wstring g_engine_dir;
std::atomic<int> g_clients{ 0 };
std::atomic<DWORD> g_last_activity{ 0 };
std::atomic<bool> g_shutdown{ false };
std::atomic<DWORD> g_engine_pid{ 0 };
std::mutex g_engine_mutex;   // serialises parameter application + speak

// Last parameters applied to the engine (so unchanged values are not resent).
struct Applied {
    bool valid = false;
    int gender = 0, pitch = 0, speed = 0, volume = 0, bright = 0;
    unsigned modes = 0;
} g_applied;

void log_line(const char* s) { CT_LOG("%s", s); }

// ---------------------------------------------------------------------------
// message-box watchdog: the engine reports fatal errors with modal dialogs;
// log their text and dismiss them so nothing ever blocks silently.
// ---------------------------------------------------------------------------
BOOL CALLBACK enum_child_text(HWND h, LPARAM lp) {
    char cls[64], txt[512];
    GetClassNameA(h, cls, sizeof(cls));
    if (_stricmp(cls, "Static") == 0) {
        GetWindowTextA(h, txt, sizeof(txt));
        if (txt[0]) {
            std::string* s = reinterpret_cast<std::string*>(lp);
            if (!s->empty()) *s += " | ";
            *s += txt;
        }
    }
    return TRUE;
}

BOOL CALLBACK enum_top(HWND h, LPARAM) {
    char cls[64];
    GetClassNameA(h, cls, sizeof(cls));
    if (strcmp(cls, "#32770") != 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != g_engine_pid.load()) return TRUE;
    char title[256];
    GetWindowTextA(h, title, sizeof(title));
    std::string body;
    EnumChildWindows(h, enum_child_text, reinterpret_cast<LPARAM>(&body));
    CT_LOG("watchdog: engine message box dismissed: \"%s\" - %s", title, body.c_str());
    PostMessageA(h, WM_COMMAND, IDOK, 0);
    PostMessageA(h, WM_CLOSE, 0, 0);
    return TRUE;
}

DWORD WINAPI watchdog_thread(LPVOID) {
    while (!g_shutdown) {
        if (g_engine_pid) EnumWindows(enum_top, 0);
        Sleep(200);
    }
    return 0;
}

// ---------------------------------------------------------------------------
std::wstring exe_dir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    size_t slash = s.find_last_of(L'\\');
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

bool file_exists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring find_engine_dir() {
    std::wstring base = exe_dir();
    const wchar_t* candidates[] = { L"\\engine", L"", L"\\bin", L"\\..\\bin", L"\\..\\engine" };
    for (const wchar_t* c : candidates) {
        std::wstring d = base + c;
        if (file_exists(d + L"\\STLTTS.EXE")) {
            wchar_t full[MAX_PATH];
            GetFullPathNameW(d.c_str(), MAX_PATH, full, nullptr);
            return full;
        }
    }
    return L"";
}

bool ensure_engine() {
    if (g_engine.running()) return true;
    std::string err;
    CT_LOG("engine not running, starting from %S", g_engine_dir.c_str());
    bool ok = g_engine.start(g_engine_dir, &err);
    g_applied.valid = false;
    if (ok) {
        g_engine_pid = g_engine.engine_pid();
    } else {
        CT_LOG("engine start failed: %s", err.c_str());
    }
    return ok;
}

// ---------------------------------------------------------------------------
// pipe helpers
// ---------------------------------------------------------------------------
bool write_all(HANDLE pipe, const void* p, uint32_t n) {
    const char* b = static_cast<const char*>(p);
    while (n > 0) {
        DWORD w = 0;
        if (!WriteFile(pipe, b, n, &w, nullptr) || w == 0) return false;
        b += w;
        n -= w;
    }
    return true;
}

bool read_all(HANDLE pipe, void* p, uint32_t n) {
    char* b = static_cast<char*>(p);
    while (n > 0) {
        DWORD r = 0;
        if (!ReadFile(pipe, b, n, &r, nullptr) || r == 0) return false;
        b += r;
        n -= r;
    }
    return true;
}

bool send_msg(HANDLE pipe, uint32_t type, const void* payload, uint32_t size) {
    PipeMessageHeader h = { type, size };
    if (!write_all(pipe, &h, sizeof(h))) return false;
    if (size && !write_all(pipe, payload, size)) return false;
    return true;
}

bool send_error(HANDLE pipe, uint32_t hr, const char* msg) {
    ErrorResponse e = {};
    e.hresult = hr;
    strncpy(e.message, msg, sizeof(e.message) - 1);
    return send_msg(pipe, RESP_ERROR, &e, sizeof(e));
}

// ---------------------------------------------------------------------------
// speak
// ---------------------------------------------------------------------------
struct PipeSink : stl::SpeakSink {
    HANDLE pipe;
    float sonic_speed = 1.0f;
    sonicStream sonic = nullptr;
    unsigned long long bytes_in = 0;      // engine bytes received
    unsigned long long bytes_out = 0;     // bytes sent to the client
    unsigned long long ts_start = 0;
    bool ts_start_known = false;
    bool pipe_broken = false;
    bool stop_requested = false;
    std::vector<short> tmp;

    explicit PipeSink(HANDLE p) : pipe(p) {}
    ~PipeSink() override { if (sonic) sonicDestroyStream(sonic); }

    bool send_audio(const void* data, uint32_t size) {
        if (!send_msg(pipe, RESP_AUDIO, data, size)) {
            pipe_broken = true;
            return false;
        }
        bytes_out += size;
        return true;
    }

    bool check_stop() {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) {
            pipe_broken = true;
            return false;
        }
        while (avail >= sizeof(PipeMessageHeader)) {
            PipeMessageHeader h;
            if (!read_all(pipe, &h, sizeof(h))) { pipe_broken = true; return false; }
            std::vector<char> payload(h.size);
            if (h.size && !read_all(pipe, payload.data(), h.size)) { pipe_broken = true; return false; }
            if (h.type == CMD_STOP) {
                CT_LOG("speak: stop requested by client after %llu bytes", bytes_out);
                stop_requested = true;
                send_msg(pipe, RESP_OK, nullptr, 0);
                return false;
            }
            CT_LOG("speak: unexpected command %lu while speaking, ignored", (unsigned long)h.type);
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) { pipe_broken = true; return false; }
        }
        return true;
    }

    bool on_audio(const void* data, unsigned size) override {
        bytes_in += size;
        if (stop_requested || pipe_broken) return false;
        if (!check_stop()) return false;
        if (sonic) {
            sonicWriteShortToStream(sonic, static_cast<const short*>(const_cast<void*>(data)), static_cast<int>(size / 2));
            int avail;
            while ((avail = sonicSamplesAvailable(sonic)) > 0) {
                tmp.resize(static_cast<size_t>(avail));
                int got = sonicReadShortFromStream(sonic, tmp.data(), avail);
                if (got > 0 && !send_audio(tmp.data(), static_cast<uint32_t>(got) * 2)) return false;
            }
            return true;
        }
        return send_audio(data, size);
    }
    void flush_sonic() {
        if (!sonic) return;
        sonicFlushStream(sonic);
        int avail;
        while ((avail = sonicSamplesAvailable(sonic)) > 0) {
            tmp.resize(static_cast<size_t>(avail));
            int got = sonicReadShortFromStream(sonic, tmp.data(), avail);
            if (got > 0 && !send_audio(tmp.data(), static_cast<uint32_t>(got) * 2)) return;
        }
    }
    uint64_t map_offset(unsigned long long ts) const {
        unsigned long long rel = ts_start_known && ts >= ts_start ? ts - ts_start : 0;
        if (sonic_speed != 1.0f) rel = static_cast<unsigned long long>(rel / sonic_speed);
        return rel & ~1ull;
    }
    void on_started(unsigned long long ts) override {
        ts_start = ts;
        ts_start_known = true;
    }
    void on_mark(unsigned mark, unsigned long long ts, unsigned long long) override {
        if (stop_requested || pipe_broken) return;
        MarkResponse m = { mark, map_offset(ts) };
        if (!send_msg(pipe, RESP_MARK, &m, sizeof(m))) pipe_broken = true;
    }
    void on_word_position(unsigned pos, unsigned long long ts, unsigned long long) override {
        if (stop_requested || pipe_broken) return;
        MarkResponse m = { pos, map_offset(ts) };
        if (!send_msg(pipe, RESP_WORD, &m, sizeof(m))) pipe_broken = true;
    }
};

bool apply_params(const SpeakCommand& c) {
    bool ok = true;
    int gender = c.gender == 1 ? 1 : 2;
    if (!g_applied.valid || g_applied.gender != gender) {
        HRESULT hr = g_engine.set_gender(gender);
        if (FAILED(hr)) { CT_LOG("set_gender(%d) failed 0x%08lX", gender, hr); ok = false; }
        g_applied.gender = gender;
        // a gender change restores that voice's default pitch and speed
        g_applied.pitch = g_applied.speed = -1;
    }
    int pmin = gender == 1 ? stl::PITCH_FEMALE_MIN : stl::PITCH_MALE_MIN;
    int pmax = gender == 1 ? stl::PITCH_FEMALE_MAX : stl::PITCH_MALE_MAX;
    int pitch = static_cast<int>(c.pitch);
    if (pitch < pmin) pitch = pmin;
    if (pitch > pmax) pitch = pmax;
    if (!g_applied.valid || g_applied.pitch != pitch) {
        HRESULT hr = g_engine.set_pitch(pitch);
        if (FAILED(hr)) { CT_LOG("set_pitch(%d) failed 0x%08lX", pitch, hr); ok = false; }
        g_applied.pitch = pitch;
    }
    int speed = static_cast<int>(c.speed);
    if (speed < stl::SPEED_MIN) speed = stl::SPEED_MIN;
    if (speed > stl::SPEED_MAX) speed = stl::SPEED_MAX;
    if (!g_applied.valid || g_applied.speed != speed) {
        HRESULT hr = g_engine.set_speed(speed);
        if (FAILED(hr)) { CT_LOG("set_speed(%d) failed 0x%08lX", speed, hr); ok = false; }
        g_applied.speed = speed;
    }
    int volume = static_cast<int>(c.volume > 255 ? 255 : c.volume);
    if (!g_applied.valid || g_applied.volume != volume) {
        HRESULT hr = g_engine.set_volume(volume);
        if (FAILED(hr)) { CT_LOG("set_volume(%d) failed 0x%08lX", volume, hr); ok = false; }
        g_applied.volume = volume;
    }
    int bright = static_cast<int>(c.bright > 15 ? 15 : c.bright);
    if (!g_applied.valid || g_applied.bright != bright) {
        HRESULT hr = g_engine.set_bright(bright);
        if (FAILED(hr)) { CT_LOG("set_bright(%d) failed 0x%08lX", bright, hr); ok = false; }
        g_applied.bright = bright;
    }
    unsigned modes = c.modes & ~MODE_FLAG_JAPANESE;   // Japanese grouping crashes the engine on mixed text
    if (!g_applied.valid || g_applied.modes != modes) {
        struct { unsigned flag; int index; } map[] = {
            { MODE_FLAG_SPREADSHEET, stl::MODE_SPREADSHEET }, { MODE_FLAG_LIST, stl::MODE_LIST },
            { MODE_FLAG_DOLLAR, stl::MODE_DOLLAR }, { MODE_FLAG_ZERO, stl::MODE_ZERO },
            { MODE_FLAG_NUMBER_SAMPLES, stl::MODE_NUMBER_SAMPLES },
        };
        for (auto& m : map) {
            int want = (modes & m.flag) ? 1 : 0;
            if (!g_applied.valid || ((g_applied.modes & m.flag) ? 1 : 0) != want) {
                HRESULT hr = g_engine.set_mode(m.index, want);
                if (FAILED(hr)) { CT_LOG("set_mode(%d,%d) failed 0x%08lX", m.index, want, hr); ok = false; }
            }
        }
        g_applied.modes = modes;
    }
    g_applied.valid = ok;
    return ok;
}

void handle_speak(HANDLE pipe, const std::vector<char>& payload) {
    if (payload.size() < sizeof(SpeakCommand)) {
        send_error(pipe, E_INVALIDARG, "speak payload too small");
        return;
    }
    SpeakCommand cmd;
    memcpy(&cmd, payload.data(), sizeof(cmd));
    if (payload.size() < sizeof(SpeakCommand) + cmd.text_length) {
        send_error(pipe, E_INVALIDARG, "speak text truncated");
        return;
    }
    std::string text(payload.data() + sizeof(SpeakCommand), cmd.text_length);

    std::lock_guard<std::mutex> lock(g_engine_mutex);
    if (!ensure_engine()) {
        send_error(pipe, E_FAIL, "CyberTalk engine could not be started");
        return;
    }
    DWORD t0 = GetTickCount();
    CT_LOG("speak: gender=%lu pitch=%lu speed=%lu volume=%lu bright=%lu modes=0x%lX sonic=%.2f text(%u)=\"%.120s%s\"",
           (unsigned long)cmd.gender, (unsigned long)cmd.pitch, (unsigned long)cmd.speed, (unsigned long)cmd.volume,
           (unsigned long)cmd.bright, (unsigned long)cmd.modes, cmd.sonic_speed, (unsigned)text.size(), text.c_str(),
           text.size() > 120 ? "..." : "");
    apply_params(cmd);

    PipeSink sink(pipe);
    if (cmd.sonic_speed > 0.0f && std::fabs(cmd.sonic_speed - 1.0f) > 0.01f) {
        sink.sonic_speed = cmd.sonic_speed;
        sink.sonic = sonicCreateStream(stl::SAMPLE_RATE, 1);
        if (sink.sonic) {
            sonicSetSpeed(sink.sonic, cmd.sonic_speed);
            sonicSetQuality(sink.sonic, 1);
        }
    }
    HRESULT hr = S_OK;
    if (!text.empty()) hr = g_engine.speak(text, sink);
    if (!sink.pipe_broken && !sink.stop_requested) sink.flush_sonic();
    // the text may have carried tags that change engine state persistently
    if (text.find('\\') != std::string::npos) g_applied.valid = false;

    SpeakEndResponse end = {};
    end.hresult = static_cast<uint32_t>(hr);
    end.total_bytes = sink.bytes_out;
    if (sink.stop_requested || hr == S_FALSE) end.status = 1;
    else if (FAILED(hr)) end.status = 2;
    else end.status = 0;
    CT_LOG("speak: %s in %lu ms, engine bytes %llu, sent %llu, hr 0x%08lX",
           end.status == 0 ? "completed" : end.status == 1 ? "cancelled" : "FAILED", GetTickCount() - t0,
           sink.bytes_in, sink.bytes_out, hr);
    if (!sink.pipe_broken) send_msg(pipe, RESP_SPEAK_END, &end, sizeof(end));
}

void fill_info(InfoResponse& i) {
    i = InfoResponse{};
    i.protocol_version = CYBERTALK_PROTOCOL_VERSION;
    i.sample_rate = stl::SAMPLE_RATE;
    i.bits_per_sample = stl::BITS_PER_SAMPLE;
    i.channels = stl::CHANNELS;
    int v[3] = {};
    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        if (ensure_engine()) g_engine.get_version(v);
    }
    for (int k = 0; k < 3; k++) i.engine_version[k] = static_cast<uint32_t>(v[k]);
    i.speed_min = stl::SPEED_MIN; i.speed_max = stl::SPEED_MAX; i.speed_default = stl::SPEED_DEFAULT;
    i.pitch_male_min = stl::PITCH_MALE_MIN; i.pitch_male_max = stl::PITCH_MALE_MAX; i.pitch_male_default = stl::PITCH_MALE_DEFAULT;
    i.pitch_female_min = stl::PITCH_FEMALE_MIN; i.pitch_female_max = stl::PITCH_FEMALE_MAX; i.pitch_female_default = stl::PITCH_FEMALE_DEFAULT;
    i.volume_max = stl::VOLUME_MAX; i.volume_default = stl::VOLUME_DEFAULT;
    i.bright_max = stl::BRIGHT_MAX; i.bright_male_default = stl::BRIGHT_MALE_DEFAULT; i.bright_female_default = stl::BRIGHT_FEMALE_DEFAULT;
    i.host_pid = GetCurrentProcessId();
    i.engine_pid = g_engine.engine_pid();
}

DWORD WINAPI client_thread(LPVOID p) {
    HANDLE pipe = static_cast<HANDLE>(p);
    g_clients++;
    CT_LOG("client connected (%d active)", g_clients.load());
    for (;;) {
        PipeMessageHeader h;
        if (!read_all(pipe, &h, sizeof(h))) break;
        std::vector<char> payload(h.size);
        if (h.size && !read_all(pipe, payload.data(), h.size)) break;
        g_last_activity = GetTickCount();
        bool ok = true;
        switch (h.type) {
        case CMD_PING:
            ok = send_msg(pipe, RESP_PONG, nullptr, 0);
            break;
        case CMD_GET_INFO: {
            InfoResponse i;
            fill_info(i);
            ok = send_msg(pipe, RESP_INFO, &i, sizeof(i));
            break;
        }
        case CMD_GET_STATE: {
            StateResponse st = {};
            {
                std::lock_guard<std::mutex> lock(g_engine_mutex);
                if (ensure_engine()) {
                    stl::EngineState es;
                    g_engine.get_state(es);
                    st.gender = es.gender; st.pitch = es.pitch; st.speed = es.speed;
                    st.volume = es.volume; st.bright = es.bright; st.number_samples = es.number_samples;
                    for (int k = 0; k < 7; k++) st.modes[k] = es.modes[k];
                }
            }
            ok = send_msg(pipe, RESP_STATE, &st, sizeof(st));
            break;
        }
        case CMD_SPEAK:
            handle_speak(pipe, payload);
            break;
        case CMD_STOP:
            ok = send_msg(pipe, RESP_OK, nullptr, 0);   // nothing in progress on this connection
            break;
        case CMD_SHUTDOWN:
            CT_LOG("shutdown requested by client");
            send_msg(pipe, RESP_OK, nullptr, 0);
            g_shutdown = true;
            break;
        default:
            ok = send_error(pipe, E_INVALIDARG, "unknown command");
            break;
        }
        if (!ok || g_shutdown) break;
    }
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    g_clients--;
    g_last_activity = GetTickCount();
    CT_LOG("client disconnected (%d active)", g_clients.load());
    return 0;
}

DWORD WINAPI idle_thread(LPVOID) {
    while (!g_shutdown) {
        Sleep(5000);
        if (g_clients == 0 && GetTickCount() - g_last_activity > IDLE_EXIT_MS) {
            CT_LOG("no clients for %lu s, exiting", IDLE_EXIT_MS / 1000);
            g_shutdown = true;
            // wake the accept loop
            HANDLE h = CreateFileW(CYBERTALK_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    ctlog::Logger::instance().init(L"host");
    HANDLE mutex = CreateMutexW(nullptr, TRUE, CYBERTALK_HOST_MUTEX);
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CT_LOG("another host is already running, exiting");
        CloseHandle(mutex);
        return 0;
    }
    CT_LOG("=== CyberTalk host starting (pid %lu) ===", GetCurrentProcessId());
    g_engine_dir = find_engine_dir();
    if (g_engine_dir.empty()) {
        CT_LOG("engine directory not found next to %S", exe_dir().c_str());
        CloseHandle(mutex);
        return 2;
    }
    CT_LOG("engine directory: %S", g_engine_dir.c_str());
    g_engine.set_logger(log_line);
    g_engine.set_verbose(false);
    g_last_activity = GetTickCount();
    CreateThread(nullptr, 0, watchdog_thread, nullptr, 0, nullptr);
    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        ensure_engine();
    }
    CreateThread(nullptr, 0, idle_thread, nullptr, 0, nullptr);

    while (!g_shutdown) {
        HANDLE pipe = CreateNamedPipeW(CYBERTALK_PIPE_NAME, PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                       PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            CT_LOG("CreateNamedPipe failed %lu", GetLastError());
            Sleep(1000);
            continue;
        }
        BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected || g_shutdown) {
            CloseHandle(pipe);
            continue;
        }
        HANDLE th = CreateThread(nullptr, 0, client_thread, pipe, 0, nullptr);
        if (th) CloseHandle(th);
        else CloseHandle(pipe);
    }
    // wait briefly for clients to finish
    for (int i = 0; i < 20 && g_clients > 0; i++) Sleep(100);
    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        g_engine.stop();
    }
    CT_LOG("=== CyberTalk host exiting ===");
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
