#include "stl_engine.h"

#include <tlhelp32.h>
#include <psapi.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdarg>

namespace stl {

namespace {

// ---------------------------------------------------------------------------
// Shared control block "stlttsmem" (0x218 bytes).  Offsets recovered from
// TTSAPI.DLL; the engine reads and writes the same layout.
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct GlueShm {
    DWORD write_idx;          // 0x000 next text slot the client fills
    DWORD read_idx;           // 0x004 next slot the engine consumes
    DWORD pending;            // 0x008 queued text buffers
    DWORD txtdd_flag;         // 0x00c "text data direct" mode flag
    DWORD notify_enabled;     // 0x010 client registered an audio start/stop sink
    DWORD text_handles[50];   // 0x014 mapping handles valid in the engine process
    DWORD init_result;        // 0x0dc ReturnInitResult
    DWORD safe_to_exit;       // 0x0e0 ReturnSafeToExit
    WORD pitch;               // 0x0e4 ReturnPitch
    WORD pad_e6;
    DWORD speed;              // 0x0e8 ReturnSpeed
    DWORD volume;             // 0x0ec ReturnVolume
    WORD bright;              // 0x0f0 ReturnBright
    WORD pad_f2;
    DWORD gender;             // 0x0f4 ReturnGender
    DWORD number;             // 0x0f8 ReturnNumber
    DWORD mode_value;         // 0x0fc ReturnMode
    DWORD is_loaded;          // 0x100 ReturnIsLoaded
    DWORD check_phoneme;      // 0x104 ReturnCheckPhoneme
    DWORD ffrw_size;          // 0x108 ReturnFFRWSize
    DWORD version[3];         // 0x10c ReturnVersion
    DWORD pos;                // 0x118 TtsGetPos (part of speech for lexicon adds)
    DWORD phoneme_handle;     // 0x11c
    DWORD phoneme_offset;     // 0x120
    DWORD phoneme_result;     // 0x124
    DWORD phoneme_size;       // 0x128
    DWORD client_busy;        // 0x12c ClientSentinel
    DWORD command;            // 0x130 notification / audio-object command code
    DWORD pad_134;
    DWORD timestamp_lo;       // 0x138
    DWORD timestamp_hi;       // 0x13c
    DWORD param;              // 0x140 flags / mark / word pos / audio size / free space
    WORD peak;                // 0x144 ReturnPeakToClient
    WORD pad_146;
    DWORD eof_flag;           // 0x148 FreeSpace EOF out
    GUID iid;                 // 0x14c QueryInterface IID from the engine's audio proxy
    DWORD result;             // 0x15c SetResultReturn / ReturnError / audio-object result
    BYTE waveformat[0x14];    // 0x160 WAVEFORMATEX (18 bytes) from WaveFormatSet
    DWORD client_count;       // 0x174
    DWORD clients[4];         // 0x178
    DWORD misc_188;           // 0x188
    DWORD misc_18c;           // 0x18c
    DWORD dialog_hwnd;        // 0x190
    BYTE pad_tail[0x218 - 0x194];
};
#pragma pack(pop)
static_assert(sizeof(GlueShm) == 0x218, "GlueShm layout");

constexpr DWORD SHM_SIZE = 0x218;
constexpr DWORD AUDIO_BUF_SIZE = 0x6590;   // 26000 bytes
constexpr DWORD SPELLING_BUF_SIZE = 0x28;
constexpr DWORD PHONEME_BUF_SIZE = 0x38;
constexpr DWORD TEXT_SLOTS = 50;

// audio-object commands (engine -> client, via audioObjectCall)
constexpr DWORD AO_QUERYINTERFACE = 0x4ce;
constexpr DWORD AO_CLAIM = 0x4e2;
constexpr DWORD AO_FLUSH = 0x4e3;
constexpr DWORD AO_START = 0x4e8;
constexpr DWORD AO_STOP = 0x4e9;
constexpr DWORD AO_UNCLAIM = 0x4ec;
constexpr DWORD AO_WAVEFORMATSET = 0x4ee;
constexpr DWORD AO_DATASET = 0x4f0;
constexpr DWORD AO_FREESPACE = 0x4f1;

// buffer notifications (engine -> client, via bufNotifyEvent)
constexpr DWORD BN_STARTED = 0x4b5;
constexpr DWORD BN_DONE = 0x4b6;
constexpr DWORD BN_WORDPOS = 0x4ba;
constexpr DWORD BN_BOOKMARK = 0x4bb;

// audio notifications (engine -> client, via NotifyEvent)
constexpr DWORD NT_AUDIOSTART = 0x4b0;
constexpr DWORD NT_AUDIOSTOP = 0x4b1;
constexpr DWORD NT_ATTRIBCHANGED = 0x4b3;

// IIDs the engine's audio proxy asks about
const GUID IID_IAudio_ = { 0xf546b340, 0xc743, 0x11cd, { 0x80, 0xe5, 0x0, 0xaa, 0x0, 0x3e, 0x4b, 0x50 } };
const GUID IID_IAudioDest_ = { 0x2ec34da0, 0xc743, 0x11cd, { 0x80, 0xe5, 0x0, 0xaa, 0x0, 0x3e, 0x4b, 0x50 } };
const GUID IID_IUnknown_ = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

enum Msg {
    M_TXTDTA, M_AUDRES, M_AUDPAU, M_AUDRSM, M_LEVEL, M_TXTDD, M_FEM, M_MAL, M_GEND, M_GVOL, M_VOL,
    M_GPIT, M_PIT, M_GSPD, M_SPD, M_GBRGHT, M_BRGHT, M_METR, M_MDE, M_GMDE, M_SNMB, M_GNMB,
    M_FFRW, M_FRWN, M_GFRM, M_SFRM, M_LMLE, M_LFML, M_LNUM, M_ULML, M_ULFM, M_ULNU,
    M_IMLO, M_IFLO, M_INLO, M_ADEN, M_DLEN, M_RSDT, M_PHON, M_CKLT, M_GTVE, M_COUNT
};
const char* const MSG_NAMES[M_COUNT] = {
    "TXTDTA", "AUDRES", "AUDPAU", "AUDRSM", "LEVEL", "TXTDD", "FEM", "MAL", "GEND", "GVOL", "VOL",
    "GPIT", "PIT", "GSPD", "SPD", "GBRGHT", "BRGHT", "METR", "MDE", "GMDE", "SNMB", "GNMB",
    "FFRW", "FRWN", "GFRM", "SFRM", "LMLE", "LFML", "LNUM", "ULML", "ULFM", "ULNU",
    "IMLO", "IFLO", "INLO", "ADEN", "DLEN", "RSDT", "PHON", "CKLT", "GTVE"
};

constexpr DWORD ACK_TIMEOUT_MS = 3000;
constexpr DWORD INIT_TIMEOUT_MS = 20000;

DWORD find_process_by_name(const wchar_t* exe, DWORD skip_pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID != skip_pid && _wcsicmp(pe.szExeFile, exe) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

}  // namespace

// ---------------------------------------------------------------------------
struct Engine::Impl {
    LogFn log_fn;
    bool verbose = false;
    unsigned clock_scale = 200;
    bool clock_patched = false;
    std::wstring dir;

    // kernel objects
    HANDLE shm_map = nullptr;
    GlueShm* shm = nullptr;
    HANDLE audio_map = nullptr;
    BYTE* audio_buf = nullptr;
    HANDLE spelling_map = nullptr;
    HANDLE phoneme_map = nullptr;
    HANDLE sem_sentinel = nullptr;
    HANDLE sem_buffer = nullptr;
    HANDLE ev_bufnotify = nullptr;
    HANDLE ev_bufnotify_ret = nullptr;
    HANDLE ev_notify = nullptr;
    HANDLE ev_notify_ret = nullptr;
    HANDLE ev_meter = nullptr;
    HANDLE ev_audio_call = nullptr;
    HANDLE ev_audio_ret = nullptr;
    HANDLE ev_stop = nullptr;       // internal: tells worker threads to exit
    HANDLE ev_done = nullptr;       // internal: TextDataDone arrived
    HANDLE ev_audio_stop = nullptr; // internal: AudioStop arrived
    HANDLE th_audio = nullptr;
    HANDLE th_bufnotify = nullptr;
    HANDLE th_notify = nullptr;

    PROCESS_INFORMATION pi = {};
    UINT msg[M_COUNT] = {};

    std::mutex cmd_mutex;      // serialises command/ack round trips
    std::mutex speak_mutex;    // serialises speak()
    std::atomic<bool> alive{ false };
    std::atomic<bool> stopping{ false };

    // per-utterance state
    std::atomic<SpeakSink*> sink{ nullptr };
    std::atomic<unsigned long long> bytes{ 0 };
    std::atomic<bool> abort_flag{ false };
    std::atomic<bool> abort_posted{ false };
    std::atomic<bool> need_buffer_wait{ false };
    std::atomic<bool> speaking{ false };
    DWORD done_flags = 0;
    unsigned long long done_ts = 0;

    WAVEFORMATEX fmt = {};
    std::atomic<bool> fmt_set{ false };

    // audio call statistics
    std::atomic<unsigned> datasets{ 0 };

    void log(const char* fmt_, ...) {
        if (!log_fn) return;
        char buf[2048];
        va_list ap;
        va_start(ap, fmt_);
        vsnprintf(buf, sizeof(buf), fmt_, ap);
        va_end(ap);
        log_fn(buf);
    }

    // ------------------------------------------------------------------ setup
    bool create_objects(std::string* err) {
        SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        auto fail = [&](const char* what) {
            char b[256];
            snprintf(b, sizeof(b), "%s failed (error %lu)", what, GetLastError());
            log("start: %s", b);
            if (err) *err = b;
            return false;
        };
        sem_sentinel = CreateSemaphoreA(nullptr, 0, 1, "SentinelSemaphore");
        if (!sem_sentinel) return fail("CreateSemaphore(SentinelSemaphore)");
        sem_buffer = CreateSemaphoreA(nullptr, 0, 1, "BufferSemaphore");
        if (!sem_buffer) return fail("CreateSemaphore(BufferSemaphore)");

        shm_map = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, SHM_SIZE, "stlttsmem");
        if (!shm_map) return fail("CreateFileMapping(stlttsmem)");
        if (GetLastError() == ERROR_ALREADY_EXISTS) log("start: warning: stlttsmem already existed (stale engine?)");
        shm = static_cast<GlueShm*>(MapViewOfFile(shm_map, FILE_MAP_WRITE, 0, 0, 0));
        if (!shm) return fail("MapViewOfFile(stlttsmem)");
        memset(shm, 0, SHM_SIZE);
        shm->result = E_FAIL;
        shm->client_count = 1;
        shm->clients[0] = 1;
        shm->notify_enabled = 1;   // we service NotifyEvent (AudioStart/AudioStop)

        spelling_map = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, SPELLING_BUF_SIZE, "spellingBuf");
        if (!spelling_map) return fail("CreateFileMapping(spellingBuf)");
        phoneme_map = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, PHONEME_BUF_SIZE, "phonemeBuf");
        if (!phoneme_map) return fail("CreateFileMapping(phonemeBuf)");
        audio_map = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, AUDIO_BUF_SIZE, "audioBuf");
        if (!audio_map) return fail("CreateFileMapping(audioBuf)");
        audio_buf = static_cast<BYTE*>(MapViewOfFile(audio_map, FILE_MAP_WRITE, 0, 0, 0));
        if (!audio_buf) return fail("MapViewOfFile(audioBuf)");

        // auto-reset request events, exactly as the original client creates them
        ev_bufnotify = CreateEventA(nullptr, FALSE, FALSE, "bufNotifyEvent");
        ev_bufnotify_ret = CreateEventA(nullptr, FALSE, FALSE, "bufNotifyReturn");
        ev_notify = CreateEventA(nullptr, FALSE, FALSE, "NotifyEvent");
        ev_notify_ret = CreateEventA(nullptr, FALSE, FALSE, "notifyReturn");
        ev_meter = CreateEventA(nullptr, FALSE, FALSE, "audioMeterEvent");
        // the engine sets audioObjectCall then resets it immediately (manual-reset
        // as in the original); audioObjectReturn is auto-reset here so that the
        // engine can never miss our acknowledgement.
        ev_audio_call = CreateEventA(nullptr, TRUE, FALSE, "audioObjectCall");
        ev_audio_ret = CreateEventA(nullptr, FALSE, FALSE, "audioObjectReturn");
        if (!ev_bufnotify || !ev_bufnotify_ret || !ev_notify || !ev_notify_ret || !ev_meter || !ev_audio_call || !ev_audio_ret)
            return fail("CreateEvent");
        ev_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ev_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ev_audio_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        for (int i = 0; i < M_COUNT; i++) {
            msg[i] = RegisterWindowMessageA(MSG_NAMES[i]);
            if (!msg[i]) return fail("RegisterWindowMessage");
        }
        return true;
    }

    void close_objects() {
        auto closeh = [](HANDLE& h) { if (h) { CloseHandle(h); h = nullptr; } };
        if (shm) { UnmapViewOfFile(shm); shm = nullptr; }
        if (audio_buf) { UnmapViewOfFile(audio_buf); audio_buf = nullptr; }
        closeh(shm_map); closeh(audio_map); closeh(spelling_map); closeh(phoneme_map);
        closeh(sem_sentinel); closeh(sem_buffer);
        closeh(ev_bufnotify); closeh(ev_bufnotify_ret); closeh(ev_notify); closeh(ev_notify_ret);
        closeh(ev_meter); closeh(ev_audio_call); closeh(ev_audio_ret);
        closeh(ev_stop); closeh(ev_done); closeh(ev_audio_stop);
    }

    // --------------------------------------------------------------- threads
    // ---------------------------------------------------- hang detection
    // The engine occasionally stops mid-utterance (it never comes back with
    // another handshake and ignores every command).  Every handshake with the
    // engine stamps last_activity; while one of our threads is inside a
    // handshake (possibly blocked in the sink, e.g. on SAPI back-pressure)
    // in_handshake is set and the gap does not count.
    std::atomic<DWORD> last_activity{ 0 };
    std::atomic<int> in_handshake{ 0 };
    struct HsEvent { DWORD tick; DWORD channel; DWORD cmd; DWORD param; };
    HsEvent hs_ring[32] = {};
    std::atomic<unsigned> hs_count{ 0 };
    static constexpr DWORD HANG_GRACE_MS = 3000;

    void note_handshake(DWORD channel, DWORD cmd, DWORD param) {
        last_activity = GetTickCount();
        unsigned n = hs_count++;
        hs_ring[n % 32] = { last_activity.load(), channel, cmd, param };
    }
    void dump_handshakes() {
        GlueShm* s = shm;
        if (s) log("engine state: command=0x%lX param=%lu result=0x%08lX pending=%lu write_idx=%lu read_idx=%lu client_busy=%lu ts=%lu",
                   s->command, s->param, s->result, s->pending, s->write_idx, s->read_idx, s->client_busy, s->timestamp_lo);
        unsigned n = hs_count.load();
        unsigned from = n > 32 ? n - 32 : 0;
        DWORD now = GetTickCount();
        for (unsigned i = from; i < n; i++) {
            const HsEvent& e = hs_ring[i % 32];
            log("  handshake -%lu ms: %s cmd=0x%lX param=%lu", now - e.tick,
                e.channel == 0 ? "audio " : e.channel == 1 ? "buffer" : "notify", e.cmd, e.param);
        }
    }
    // the engine is unresponsive: kill it so the owner can start a fresh one
    void declare_hung(const char* why) {
        log("ENGINE HUNG (%s): terminating STLTTS.EXE, it will be restarted", why);
        dump_handshakes();
        terminate_engine();
        alive = false;
    }

    static DWORD WINAPI dispatch_thread_proc(LPVOID p) { static_cast<Impl*>(p)->dispatch_thread(); return 0; }
    std::atomic<unsigned> lost_pulses{ 0 };

    // ----------------------------------------------------------- dispatcher
    // The engine issues every call the same way: it writes the command code
    // and its arguments into the control block, does SetEvent + ResetEvent on
    // the channel's call event (manual-reset, so the pulse is lost unless we
    // are already waiting) and then blocks on the channel's return event.
    // With the engine running 200x faster than real time the pulse is lost
    // now and then, which hangs the engine for good.  So one thread serves
    // all three channels and, when a wait times out, polls the command word:
    // a command that is still pending after two polls was a lost pulse.
    // The engine never has two calls in flight, and the three channels use
    // disjoint command codes, so the command word alone says which channel
    // to answer.  The command word is cleared before the return event is set
    // so a stale value is never served twice.
    void dispatch_thread() {
        HANDLE waits[4] = { ev_stop, ev_audio_call, ev_bufnotify, ev_notify };
        DWORD seen_cmd = 0;
        for (;;) {
            DWORD w = WaitForMultipleObjects(4, waits, FALSE, 1);
            if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) break;
            DWORD cmd = shm->command;
            if (cmd == 0) { seen_cmd = 0; continue; }
            if (w == WAIT_TIMEOUT) {
                // polled: act only once the same command has been pending for
                // two consecutive polls, so the engine has surely finished
                // writing the arguments
                if (cmd != seen_cmd) { seen_cmd = cmd; continue; }
                lost_pulses++;
                if (lost_pulses <= 5 || (lost_pulses % 100) == 0)
                    log("dispatcher: recovered a lost call pulse (command 0x%lX, %u so far)", cmd, lost_pulses.load());
            }
            seen_cmd = 0;
            in_handshake++;
            if (cmd >= AO_QUERYINTERFACE && cmd <= AO_FREESPACE) handle_audio(cmd);
            else if (cmd >= BN_STARTED && cmd <= BN_BOOKMARK) handle_bufnotify(cmd);
            else if (cmd >= NT_AUDIOSTART && cmd <= NT_ATTRIBCHANGED) handle_notify(cmd);
            else {
                log("dispatcher: unknown command 0x%lX", cmd);
                shm->command = 0;
                shm->result = static_cast<DWORD>(E_NOTIMPL);
                last_activity = GetTickCount();
                // answer whichever channel asked
                if (WaitForSingleObject(ev_audio_call, 0) == WAIT_OBJECT_0) SetEvent(ev_audio_ret);
                else if (WaitForSingleObject(ev_bufnotify, 0) == WAIT_OBJECT_0) SetEvent(ev_bufnotify_ret);
                else if (WaitForSingleObject(ev_notify, 0) == WAIT_OBJECT_0) SetEvent(ev_notify_ret);
                else { SetEvent(ev_audio_ret); SetEvent(ev_bufnotify_ret); SetEvent(ev_notify_ret); }
            }
            in_handshake--;
        }
    }

    void handle_audio(DWORD cmd) {
        note_handshake(0, cmd, shm->param);
        HRESULT res = S_OK;
        switch (cmd) {
        case AO_QUERYINTERFACE: {
            GUID g;
            memcpy(&g, &shm->iid, sizeof(GUID));
            bool known = (g == IID_IAudio_) || (g == IID_IAudioDest_) || (g == IID_IUnknown_);
            res = known ? S_OK : E_NOINTERFACE;
            if (verbose) log("audio: QueryInterface {%08lX-%04X-%04X-...} -> %s", g.Data1, g.Data2, g.Data3, known ? "S_OK" : "E_NOINTERFACE");
            break;
        }
        case AO_WAVEFORMATSET: {
            WAVEFORMATEX f = {};
            memcpy(&f, shm->waveformat, 18);
            fmt = f;
            fmt_set = true;
            log("audio: WaveFormatSet tag=%u ch=%u rate=%lu bits=%u align=%u avg=%lu",
                f.wFormatTag, f.nChannels, f.nSamplesPerSec, f.wBitsPerSample, f.nBlockAlign, f.nAvgBytesPerSec);
            break;
        }
        case AO_DATASET: {
            DWORD n = shm->param;
            if (n > AUDIO_BUF_SIZE) n = AUDIO_BUF_SIZE;
            datasets++;
            SpeakSink* s = sink.load();
            if (s && !abort_flag) {
                bool ok = s->on_audio(audio_buf, n);
                bytes += n;
                if (!ok) {
                    log("audio: sink requested cancel after %llu bytes", bytes.load());
                    request_abort();
                }
            }
            break;
        }
        case AO_FREESPACE:
            shm->param = 0x100000;
            shm->eof_flag = 0;
            break;
        case AO_CLAIM: case AO_START: case AO_STOP: case AO_UNCLAIM: case AO_FLUSH:
            if (verbose) log("audio: command 0x%lX", cmd);
            break;
        default:
            log("audio: unknown audio-object command 0x%lX", cmd);
            break;
        }
        shm->command = 0;
        shm->result = static_cast<DWORD>(res);
        last_activity = GetTickCount();
        SetEvent(ev_audio_ret);
    }

    void handle_bufnotify(DWORD cmd) {
        unsigned long long ts = (static_cast<unsigned long long>(shm->timestamp_hi) << 32) | shm->timestamp_lo;
        DWORD param = shm->param;
        note_handshake(1, cmd, param);
        SpeakSink* s = sink.load();
        switch (cmd) {
        case BN_STARTED:
            if (verbose) log("notify: TextDataStarted ts=%llu", ts);
            if (s) s->on_started(ts);
            break;
        case BN_DONE:
            if (verbose) log("notify: TextDataDone ts=%llu flags=0x%lX bytes=%llu", ts, param, bytes.load());
            done_flags = param;
            done_ts = ts;
            if (s) s->on_done(ts, param);
            SetEvent(ev_done);
            break;
        case BN_WORDPOS:
            if (s) s->on_word_position(param, ts, bytes.load());
            break;
        case BN_BOOKMARK:
            if (s) s->on_mark(param, ts, bytes.load());
            break;
        default:
            log("notify: unknown buffer notification 0x%lX", cmd);
            break;
        }
        shm->command = 0;
        last_activity = GetTickCount();
        SetEvent(ev_bufnotify_ret);
    }

    void handle_notify(DWORD cmd) {
        unsigned long long ts = (static_cast<unsigned long long>(shm->timestamp_hi) << 32) | shm->timestamp_lo;
        note_handshake(2, cmd, shm->param);
        SpeakSink* s = sink.load();
        switch (cmd) {
        case NT_AUDIOSTART:
            if (verbose) log("notify: AudioStart ts=%llu", ts);
            if (s) s->on_audio_start(ts);
            break;
        case NT_AUDIOSTOP:
            if (verbose) log("notify: AudioStop ts=%llu bytes=%llu", ts, bytes.load());
            if (s) s->on_audio_stop(ts);
            SetEvent(ev_audio_stop);
            break;
        case NT_ATTRIBCHANGED:
            if (verbose) log("notify: AttribChanged %lu", shm->param);
            break;
        default:
            log("notify: unknown notification 0x%lX", cmd);
            break;
        }
        shm->command = 0;
        last_activity = GetTickCount();
        SetEvent(ev_notify_ret);
    }

    // ------------------------------------------------------------- commands
    bool post(int m, WPARAM wp, LPARAM lp) {
        if (!alive) return false;
        if (!PostThreadMessageA(pi.dwThreadId, msg[m], wp, lp)) {
            log("PostThreadMessage(%s) failed %lu", MSG_NAMES[m], GetLastError());
            return false;
        }
        return true;
    }

    void drain_sentinel() {
        int n = 0;
        while (WaitForSingleObject(sem_sentinel, 0) == WAIT_OBJECT_0) n++;
        if (n) log("drained %d stale acknowledgement(s)", n);
    }

    // post a command and wait for the engine's acknowledgement; returns the
    // engine's result code or E_FAIL on timeout.
    // has_result: the engine's acknowledgement writes shm->result (ReturnError /
    // ReturnMode); pure queries (ReturnPitch etc.) leave it untouched.
    HRESULT command(int m, WPARAM wp = 0, LPARAM lp = 0, bool has_result = true) {
        std::lock_guard<std::mutex> lock(cmd_mutex);
        if (!alive) return E_FAIL;
        drain_sentinel();
        shm->result = 0x7FFFFFFF;   // sentinel value so a stale result is detectable
        if (!post(m, wp, lp)) return E_FAIL;
        DWORD w = WaitForSingleObject(sem_sentinel, ACK_TIMEOUT_MS);
        if (w != WAIT_OBJECT_0) {
            log("command %s(%lu,%ld): no acknowledgement within %lu ms", MSG_NAMES[m], (unsigned long)wp, (long)lp, ACK_TIMEOUT_MS);
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
                log("engine process has exited");
                alive = false;
            } else {
                declare_hung("command not acknowledged");
            }
            return E_FAIL;
        }
        HRESULT r = has_result ? static_cast<HRESULT>(shm->result) : S_OK;
        if (has_result && r == 0x7FFFFFFF) {
            log("command %s(%lu,%ld): acknowledged but no result written", MSG_NAMES[m], (unsigned long)wp, (long)lp);
            r = S_OK;
        }
        if (verbose) log("command %s(%lu,%ld) -> 0x%08lX (pitch=%u speed=%lu vol=%lu bright=%u gender=%lu number=%lu mode=%lu)",
                         MSG_NAMES[m], (unsigned long)wp, (long)lp, r, shm->pitch, shm->speed, shm->volume, shm->bright, shm->gender, shm->number, shm->mode_value);
        return r;
    }

    // post a command that the engine does not acknowledge
    bool fire(int m, WPARAM wp = 0, LPARAM lp = 0) {
        std::lock_guard<std::mutex> lock(cmd_mutex);
        if (verbose) log("post %s(%lu,%ld)", MSG_NAMES[m], (unsigned long)wp, (long)lp);
        return post(m, wp, lp);
    }

    void request_abort() {
        abort_flag = true;
        if (!speaking) return;
        bool expected = false;
        if (abort_posted.compare_exchange_strong(expected, true)) {
            std::lock_guard<std::mutex> lock(cmd_mutex);
            log("abort: posting AUDRES");
            post(M_AUDRES, 0, 0);
            need_buffer_wait = true;
        }
    }

    // --------------------------------------------------------------- engine
    bool launch(std::string* err) {
        std::wstring exe = dir + L"\\STLTTS.EXE";
        SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        STARTUPINFOW si = { sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        std::wstring cmdline = L"\"" + exe + L"\"";
        std::vector<wchar_t> cl(cmdline.begin(), cmdline.end());
        cl.push_back(0);
        if (!CreateProcessW(exe.c_str(), cl.data(), &sa, &sa, TRUE,
                            NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
            char b[512];
            snprintf(b, sizeof(b), "CreateProcess(STLTTS.EXE) failed (error %lu)", GetLastError());
            log("start: %s", b);
            if (err) *err = b;
            return false;
        }
        log("start: launched STLTTS.EXE pid=%lu tid=%lu", pi.dwProcessId, pi.dwThreadId);
        return true;
    }

    // ------------------------------------------------------------ clock patch
    // STLTTS.EXE paces itself like a sound card: after synthesising an
    // utterance it spins (100% CPU) until timeGetTime() says the audio would
    // have finished playing, and only then reports bookmarks/word positions,
    // sends AudioStop and accepts the next text.  Redirect its timeGetTime
    // import to a stub returning a clock that runs `scale` times faster: the
    // wait collapses to a few milliseconds and, because the byte positions it
    // reports are derived from that same clock relative to its own start, they
    // stay exact.
    bool patch_engine_clock(unsigned scale) {
        if (scale <= 1) return false;
        const DWORD IMAGE_BASE = 0x400000, IAT_SLOT_RVA = 0x0ea3bc;   // WINMM!timeGetTime slot in STLTTS.EXE
        HMODULE mods[1] = {};
        DWORD needed = 0;
        DWORD base = IMAGE_BASE;
        if (EnumProcessModules(pi.hProcess, mods, sizeof(mods), &needed) && mods[0]) {
            MODULEINFO mi = {};
            if (GetModuleInformation(pi.hProcess, mods[0], &mi, sizeof(mi))) base = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(mi.lpBaseOfDll));
        }
        DWORD slot = base + IAT_SLOT_RVA;
        DWORD real = 0;
        SIZE_T n = 0;
        if (!ReadProcessMemory(pi.hProcess, reinterpret_cast<LPCVOID>(static_cast<ULONG_PTR>(slot)), &real, 4, &n) || n != 4) {
            log("clock patch: cannot read import slot (%lu)", GetLastError());
            return false;
        }
        HMODULE winmm = GetModuleHandleW(L"winmm.dll");
        if (!winmm) winmm = LoadLibraryW(L"winmm.dll");
        DWORD mine = winmm ? static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(GetProcAddress(winmm, "timeGetTime"))) : 0;
        if (mine && mine != real) log("clock patch: engine timeGetTime=%08lX differs from ours %08lX (using the engine's)", real, mine);
        if (!real) { log("clock patch: import slot empty"); return false; }
        // mov eax, real ; call eax ; imul eax, eax, scale ; ret
        BYTE stub[14] = { 0xB8, 0, 0, 0, 0, 0xFF, 0xD0, 0x69, 0xC0, 0, 0, 0, 0, 0xC3 };
        memcpy(stub + 1, &real, 4);
        memcpy(stub + 9, &scale, 4);
        LPVOID mem = VirtualAllocEx(pi.hProcess, nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!mem) { log("clock patch: VirtualAllocEx failed (%lu)", GetLastError()); return false; }
        if (!WriteProcessMemory(pi.hProcess, mem, stub, sizeof(stub), &n)) { log("clock patch: WriteProcessMemory(stub) failed (%lu)", GetLastError()); return false; }
        DWORD old = 0;
        VirtualProtectEx(pi.hProcess, reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(slot)), 4, PAGE_READWRITE, &old);
        DWORD val = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(mem));
        bool ok = WriteProcessMemory(pi.hProcess, reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(slot)), &val, 4, &n) && n == 4;
        if (old) VirtualProtectEx(pi.hProcess, reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(slot)), 4, old, &old);
        FlushInstructionCache(pi.hProcess, mem, 4096);
        if (!ok) { log("clock patch: WriteProcessMemory(slot) failed (%lu)", GetLastError()); return false; }
        log("clock patch: engine clock x%u (import slot %08lX -> stub %08lX, real timeGetTime %08lX)", scale, slot, val, real);
        clock_patched = true;
        return true;
    }

    void terminate_engine() {
        if (pi.hProcess) {
            if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0) {
                log("terminating engine process %lu", pi.dwProcessId);
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 2000);
            }
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        pi = PROCESS_INFORMATION{};
    }
};

// ---------------------------------------------------------------------------
Engine::Engine() : impl_(new Impl) {}
Engine::~Engine() { stop(); }

void Engine::set_logger(LogFn fn) { impl_->log_fn = std::move(fn); }
void Engine::set_verbose(bool v) { impl_->verbose = v; }
void Engine::set_clock_scale(unsigned scale) { impl_->clock_scale = scale; }
DWORD Engine::engine_pid() const { return impl_->pi.dwProcessId; }
bool Engine::aborted() const { return impl_->abort_flag; }
WAVEFORMATEX Engine::wave_format() const { return impl_->fmt; }
bool Engine::wave_format_known() const { return impl_->fmt_set; }

bool Engine::running() {
    Impl& I = *impl_;
    if (!I.alive) return false;
    if (I.pi.hProcess && WaitForSingleObject(I.pi.hProcess, 0) == WAIT_OBJECT_0) {
        I.log("engine process exited unexpectedly");
        I.alive = false;
    }
    return I.alive;
}

bool Engine::start(const std::wstring& engine_dir, std::string* error) {
    Impl& I = *impl_;
    if (I.alive) return true;
    if (I.shm || I.pi.hProcess) stop();   // engine died earlier: clean up before relaunching
    I.dir = engine_dir;
    while (!I.dir.empty() && (I.dir.back() == L'\\' || I.dir.back() == L'/')) I.dir.pop_back();
    I.stopping = false;
    I.abort_flag = false;
    I.abort_posted = false;
    I.need_buffer_wait = false;
    I.fmt_set = false;

    // A stale engine from a crashed host would hold the named objects.
    for (int i = 0; i < 8; i++) {
        DWORD stale = find_process_by_name(L"STLTTS.EXE", 0);
        if (!stale) break;
        I.log("start: killing stale STLTTS.EXE pid %lu", stale);
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, stale);
        if (h) { TerminateProcess(h, 1); WaitForSingleObject(h, 2000); CloseHandle(h); }
        Sleep(100);
    }

    if (!I.create_objects(error)) { I.close_objects(); return false; }

    I.th_audio = CreateThread(nullptr, 0, Impl::dispatch_thread_proc, &I, 0, nullptr);
    I.th_bufnotify = nullptr;
    I.th_notify = nullptr;

    DWORD t0 = GetTickCount();
    if (!I.launch(error)) { stop(); return false; }
    I.alive = true;

    // the engine releases the sentinel from ReturnInitResult once it is ready
    DWORD w = WaitForSingleObject(I.sem_sentinel, INIT_TIMEOUT_MS);
    if (w != WAIT_OBJECT_0) {
        I.log("start: engine did not report readiness within %lu ms", INIT_TIMEOUT_MS);
        if (error) *error = "engine did not start (timeout)";
        stop();
        return false;
    }
    if (I.shm->init_result != 1) {
        I.log("start: engine init failed, init_result=%ld result=0x%08lX", (long)I.shm->init_result, I.shm->result);
        if (error) {
            char b[128];
            snprintf(b, sizeof(b), "engine initialisation failed (code %ld)", (long)I.shm->init_result);
            *error = b;
        }
        stop();
        return false;
    }
    I.log("start: engine ready in %lu ms", GetTickCount() - t0);
    I.clock_patched = false;
    if (I.clock_scale > 1) I.patch_engine_clock(I.clock_scale);
    int v[3] = {};
    if (get_version(v)) I.log("start: engine version %d.%d.%d", v[0], v[1], v[2]);
    return true;
}

void Engine::stop() {
    Impl& I = *impl_;
    if (I.stopping) return;
    I.stopping = true;
    if (I.alive) {
        I.abort_flag = true;
        std::lock_guard<std::mutex> lock(I.cmd_mutex);
        I.drain_sentinel();
        I.log("stop: posting WM_QUIT to engine");
        PostThreadMessageA(I.pi.dwThreadId, WM_QUIT, 0, 0);
        DWORD w = WaitForSingleObject(I.sem_sentinel, 3000);
        I.log("stop: %s (safe_to_exit=%lu)", w == WAIT_OBJECT_0 ? "engine acknowledged exit" : "no exit acknowledgement", I.shm ? I.shm->safe_to_exit : 0);
        if (WaitForSingleObject(I.pi.hProcess, 3000) != WAIT_OBJECT_0) I.log("stop: engine still running");
        I.alive = false;
    }
    I.terminate_engine();
    if (I.ev_stop) SetEvent(I.ev_stop);
    HANDLE ths[3] = { I.th_audio, I.th_bufnotify, I.th_notify };
    for (HANDLE h : ths) {
        if (h) {
            if (WaitForSingleObject(h, 2000) != WAIT_OBJECT_0) TerminateThread(h, 0);
            CloseHandle(h);
        }
    }
    I.th_audio = I.th_bufnotify = I.th_notify = nullptr;
    I.close_objects();
    I.stopping = false;
}

// ------------------------------------------------------------------ params
HRESULT Engine::set_gender(int gender) {
    Impl& I = *impl_;
    if (gender == GENDER_FEMALE) return I.command(M_FEM);
    if (gender == GENDER_MALE) return I.command(M_MAL);
    return E_INVALIDARG;
}
HRESULT Engine::set_pitch(int hz) { return impl_->command(M_PIT, static_cast<WPARAM>(hz & 0xFFFF)); }
HRESULT Engine::set_speed(int wpm) { return impl_->command(M_SPD, static_cast<WPARAM>(wpm & 0xFFFF)); }
HRESULT Engine::set_volume(int v) {
    if (v < 0 || v > 255) return E_INVALIDARG;
    return impl_->command(M_VOL, static_cast<WPARAM>(v));
}
HRESULT Engine::set_bright(int b) {
    if (b < 0 || b > BRIGHT_MAX) return E_INVALIDARG;
    Impl& I = *impl_;
    if (!I.fire(M_BRGHT, static_cast<WPARAM>(b))) return E_FAIL;
    // BRGHT is not acknowledged; a query is, and it is processed after it.
    int now = get_bright();
    return now == b ? S_OK : (now < 0 ? E_FAIL : E_UNEXPECTED);
}
HRESULT Engine::set_number_samples(int on) {
    Impl& I = *impl_;
    if (!I.fire(M_SNMB, on ? 1 : 0)) return E_FAIL;
    int now = get_number_samples();
    return now == (on ? 1 : 0) ? S_OK : (now < 0 ? E_FAIL : E_UNEXPECTED);
}
HRESULT Engine::set_mode(int index, int value) {
    if (index < 0 || index > 6) return E_INVALIDARG;
    return impl_->command(M_MDE, static_cast<WPARAM>(index), static_cast<LPARAM>(value));
}
HRESULT Engine::load_samples(int which) {
    Impl& I = *impl_;
    int m = which == SAMPLES_MALE ? M_LMLE : which == SAMPLES_FEMALE ? M_LFML : which == SAMPLES_NUMBER ? M_LNUM : -1;
    if (m < 0) return E_INVALIDARG;
    if (!I.fire(m)) return E_FAIL;
    return is_loaded(which) == 1 ? S_OK : E_FAIL;
}
HRESULT Engine::unload_samples(int which) {
    Impl& I = *impl_;
    int m = which == SAMPLES_MALE ? M_ULML : which == SAMPLES_FEMALE ? M_ULFM : which == SAMPLES_NUMBER ? M_ULNU : -1;
    if (m < 0) return E_INVALIDARG;
    if (!I.fire(m)) return E_FAIL;
    return is_loaded(which) == 0 ? S_OK : E_FAIL;
}
int Engine::get_gender() { Impl& I = *impl_; return SUCCEEDED(I.command(M_GEND, 0, 0, false)) ? static_cast<int>(I.shm->gender) : -1; }
int Engine::get_pitch() { Impl& I = *impl_; return SUCCEEDED(I.command(M_GPIT, 0, 0, false)) ? static_cast<int>(I.shm->pitch) : -1; }
int Engine::get_speed() { Impl& I = *impl_; return SUCCEEDED(I.command(M_GSPD, 0, 0, false)) ? static_cast<int>(I.shm->speed) : -1; }
int Engine::get_volume() { Impl& I = *impl_; return SUCCEEDED(I.command(M_GVOL, 0, 0, false)) ? static_cast<int>(I.shm->volume) : -1; }
int Engine::get_bright() { Impl& I = *impl_; return SUCCEEDED(I.command(M_GBRGHT, 0, 0, false)) ? static_cast<int>(I.shm->bright) : -1; }
int Engine::get_number_samples() { Impl& I = *impl_; return SUCCEEDED(I.command(M_GNMB, 0, 0, false)) ? static_cast<int>(I.shm->number) : -1; }
int Engine::get_mode(int index) {
    Impl& I = *impl_;
    if (index < 0 || index > 6) return -1;
    return SUCCEEDED(I.command(M_GMDE, static_cast<WPARAM>(index))) ? static_cast<int>(I.shm->mode_value) : -1;
}
int Engine::is_loaded(int which) {
    Impl& I = *impl_;
    int m = which == SAMPLES_MALE ? M_IMLO : which == SAMPLES_FEMALE ? M_IFLO : which == SAMPLES_NUMBER ? M_INLO : -1;
    if (m < 0) return -1;
    return SUCCEEDED(I.command(m, 0, 0, false)) ? static_cast<int>(I.shm->is_loaded) : -1;
}
bool Engine::get_version(int out[3]) {
    Impl& I = *impl_;
    if (FAILED(I.command(M_GTVE, 0, 0, false))) return false;
    for (int i = 0; i < 3; i++) out[i] = static_cast<int>(I.shm->version[i]);
    return true;
}
bool Engine::get_state(EngineState& st) {
    st.gender = get_gender();
    st.pitch = get_pitch();
    st.speed = get_speed();
    st.volume = get_volume();
    st.bright = get_bright();
    st.number_samples = get_number_samples();
    for (int i = 0; i <= 6; i++) st.modes[i] = get_mode(i);
    get_version(st.version);
    return st.gender >= 0 && st.pitch >= 0;
}

// ------------------------------------------------------------------- speak
HRESULT Engine::speak(const std::string& text, SpeakSink& sink, DWORD timeout_ms) {
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lock(I.speak_mutex);
    if (!running()) return E_FAIL;

    // after a cancel the engine releases BufferSemaphore once it has discarded
    // the queue; the next utterance must wait for that.
    if (I.need_buffer_wait) {
        DWORD w = WaitForSingleObject(I.sem_buffer, 3000);
        I.log("speak: post-cancel BufferSemaphore %s", w == WAIT_OBJECT_0 ? "acquired" : "TIMEOUT");
        I.need_buffer_wait = false;
    }
    while (WaitForSingleObject(I.sem_buffer, 0) == WAIT_OBJECT_0) {}

    I.abort_flag = false;
    I.abort_posted = false;
    I.bytes = 0;
    I.datasets = 0;
    I.done_flags = 0;
    I.last_activity = GetTickCount();
    ResetEvent(I.ev_done);
    ResetEvent(I.ev_audio_stop);
    I.sink = &sink;
    I.speaking = true;

    HRESULT result = S_OK;
    {
        std::lock_guard<std::mutex> clock(I.cmd_mutex);
        GlueShm* shm = I.shm;
        if (shm->write_idx - shm->read_idx == 0xFFFFFFFFu) {
            I.log("speak: engine text queue full");
            I.sink = nullptr; I.speaking = false;
            return E_FAIL;
        }
        DWORD size = static_cast<DWORD>(text.size() + 1);
        SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, size + 0x10, nullptr);
        if (!map) {
            I.log("speak: CreateFileMapping(text) failed %lu", GetLastError());
            I.sink = nullptr; I.speaking = false;
            return E_OUTOFMEMORY;
        }
        HANDLE dup = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), map, I.pi.hProcess, &dup, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            I.log("speak: DuplicateHandle into engine failed %lu", GetLastError());
            CloseHandle(map);
            I.sink = nullptr; I.speaking = false;
            return E_FAIL;
        }
        void* view = MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 0);
        if (!view) {
            I.log("speak: MapViewOfFile(text) failed %lu", GetLastError());
            CloseHandle(map);
            I.sink = nullptr; I.speaking = false;
            return E_FAIL;
        }
        memcpy(view, text.c_str(), size);
        UnmapViewOfFile(view);
        CloseHandle(map);

        if (shm->txtdd_flag) shm->txtdd_flag = 0;
        shm->pending++;
        shm->text_handles[shm->write_idx] = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(dup));
        I.drain_sentinel();
        if (!I.post(M_TXTDTA, size, 0)) {
            I.sink = nullptr; I.speaking = false;
            return E_FAIL;
        }
        shm->write_idx = (shm->write_idx + 1) % TEXT_SLOTS;
        DWORD w = WaitForSingleObject(I.sem_sentinel, ACK_TIMEOUT_MS);
        if (w != WAIT_OBJECT_0) {
            I.log("speak: TXTDTA not acknowledged");
            result = E_FAIL;
        } else {
            result = static_cast<HRESULT>(shm->result);
            if (I.verbose) I.log("speak: TXTDTA accepted, %lu bytes of text, result 0x%08lX", size, result);
        }
    }
    if (FAILED(result)) {
        I.log("speak: engine rejected text: 0x%08lX", result);
        I.sink = nullptr; I.speaking = false;
        return result;
    }

    // wait for completion; AudioStop follows TextDataDone once the last audio
    // has been delivered.
    DWORD t0 = GetTickCount();
    HANDLE waits[2] = { I.ev_done, I.pi.hProcess };
    DWORD w = WAIT_TIMEOUT;
    bool hung = false;
    for (;;) {
        w = WaitForMultipleObjects(2, waits, FALSE, 100);
        if (w != WAIT_TIMEOUT) break;
        const DWORD now = GetTickCount();
        if (now - t0 > timeout_ms) break;
        if (I.in_handshake.load() == 0) {
            // the engine is neither talking to us nor blocked on us; allow for
            // the (scaled) playback time it still has to simulate before AudioStop
            const DWORD idle = now - I.last_activity.load();
            const DWORD allowed = Impl::HANG_GRACE_MS + static_cast<DWORD>(I.bytes.load() / 22 / (I.clock_patched ? I.clock_scale : 1));
            if (idle > allowed) { hung = true; break; }
        }
    }
    if (hung) {
        char why[96];
        snprintf(why, sizeof(why), "no handshake for %lu ms after %llu bytes", GetTickCount() - I.last_activity.load(), I.bytes.load());
        I.declare_hung(why);
        result = E_FAIL;
    } else if (w == WAIT_OBJECT_0 + 1) {
        I.log("speak: engine process died during speech");
        I.alive = false;
        result = E_FAIL;
    } else if (w != WAIT_OBJECT_0) {
        I.log("speak: timeout after %lu ms, cancelling", GetTickCount() - t0);
        I.request_abort();
        WaitForSingleObject(I.ev_done, 3000);
        result = E_ABORT;
    } else {
        // AudioStop arrives once the engine's (scaled) playback clock passes the
        // end of the audio; all bookmark / word position reports precede it.
        unsigned long long audio_ms = I.bytes.load() / 22;
        DWORD stop_wait = I.abort_flag ? 500 : static_cast<DWORD>(audio_ms / (I.clock_patched ? I.clock_scale : 1) + 1500);
        DWORD w2 = WaitForSingleObject(I.ev_audio_stop, stop_wait);
        if (w2 != WAIT_OBJECT_0 && !I.abort_flag) I.log("speak: AudioStop not seen within %lu ms", stop_wait);
        if (I.verbose) I.log("speak: done in %lu ms, %llu bytes in %u chunks, AudioStop %s, flags 0x%lX",
                             GetTickCount() - t0, I.bytes.load(), I.datasets.load(), w2 == WAIT_OBJECT_0 ? "seen" : "not seen", I.done_flags);
        if (I.abort_flag) result = S_FALSE;
    }
    if (I.abort_flag && I.need_buffer_wait) {
        DWORD wb = WaitForSingleObject(I.sem_buffer, 3000);
        I.log("speak: cancel settled (%s)", wb == WAIT_OBJECT_0 ? "BufferSemaphore acquired" : "TIMEOUT");
        I.need_buffer_wait = false;
    }
    I.speaking = false;
    I.sink = nullptr;
    return result;
}

void Engine::abort() {
    impl_->request_abort();
}

bool Engine::debug_take_sentinel() {
    Impl& I = *impl_;
    if (!I.sem_sentinel) return false;
    return WaitForSingleObject(I.sem_sentinel, 0) == WAIT_OBJECT_0;
}

unsigned long long Engine::engine_cpu_ms() {
    Impl& I = *impl_;
    if (!I.pi.hProcess) return 0;
    FILETIME c, e, k, u;
    if (!GetProcessTimes(I.pi.hProcess, &c, &e, &k, &u)) return 0;
    ULARGE_INTEGER uk, uu;
    uk.LowPart = k.dwLowDateTime; uk.HighPart = k.dwHighDateTime;
    uu.LowPart = u.dwLowDateTime; uu.HighPart = u.dwHighDateTime;
    return (uk.QuadPart + uu.QuadPart) / 10000ull;
}

}  // namespace stl
