// CyberTalk engine probe / sample generator.
//
//   cybertalk_probe.exe <engine dir> <output dir> [quick]
//
// Drives STLTTS.EXE directly through the recovered shared-memory protocol
// (src/stl_engine.cpp), verifies parameter ranges, exercises every discovered
// feature, and writes one WAV per test into <output dir> plus probe_report.txt.
#include "../src/stl_engine.h"
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <tlhelp32.h>

static FILE* g_report = nullptr;
static std::mutex g_log_mutex;

static void rep(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    fflush(stdout);
    if (g_report) { fprintf(g_report, "%s\n", buf); fflush(g_report); }
}

// ---------------------------------------------------------------------------
// message-box watchdog for the engine process
// ---------------------------------------------------------------------------
static std::atomic<DWORD> g_engine_pid{ 0 };

static BOOL CALLBACK enum_child_text(HWND h, LPARAM lp) {
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

static BOOL CALLBACK enum_top(HWND h, LPARAM) {
    char cls[64];
    GetClassNameA(h, cls, sizeof(cls));
    if (strcmp(cls, "#32770") != 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() && pid != g_engine_pid) return TRUE;
    char title[256];
    GetWindowTextA(h, title, sizeof(title));
    std::string body;
    EnumChildWindows(h, enum_child_text, reinterpret_cast<LPARAM>(&body));
    rep("WATCHDOG: dismissing message box (pid %lu) title=\"%s\" text=\"%s\"", pid, title, body.c_str());
    PostMessageA(h, WM_COMMAND, IDOK, 0);
    PostMessageA(h, WM_CLOSE, 0, 0);
    return TRUE;
}

static std::atomic<bool> g_watchdog_stop{ false };
static DWORD WINAPI watchdog_thread(LPVOID) {
    while (!g_watchdog_stop) {
        EnumWindows(enum_top, 0);
        Sleep(150);
    }
    return 0;
}

// ---------------------------------------------------------------------------
struct Collector : stl::SpeakSink {
    std::vector<BYTE> pcm;
    std::mutex mtx;
    unsigned chunks = 0;
    unsigned min_chunk = 0xFFFFFFFF, max_chunk = 0;
    DWORD t0 = 0, first_tick = 0, last_tick = 0;
    struct Ev { std::string kind; unsigned id; unsigned long long ts; unsigned long long bytes; DWORD tick; };
    std::vector<Ev> events;
    bool cancel_after_bytes_enabled = false;
    size_t cancel_after_bytes = 0;

    void reset() {
        std::lock_guard<std::mutex> lock(mtx);
        pcm.clear(); chunks = 0; min_chunk = 0xFFFFFFFF; max_chunk = 0;
        first_tick = last_tick = 0; events.clear();
        cancel_after_bytes_enabled = false;
        t0 = GetTickCount();
    }
    bool on_audio(const void* data, unsigned size) override {
        std::lock_guard<std::mutex> lock(mtx);
        if (!chunks) first_tick = GetTickCount();
        last_tick = GetTickCount();
        chunks++;
        if (size < min_chunk) min_chunk = size;
        if (size > max_chunk) max_chunk = size;
        const BYTE* b = static_cast<const BYTE*>(data);
        pcm.insert(pcm.end(), b, b + size);
        if (cancel_after_bytes_enabled && pcm.size() >= cancel_after_bytes) return false;
        return true;
    }
    void on_started(unsigned long long ts) override { push("started", 0, ts); }
    void on_mark(unsigned m, unsigned long long ts, unsigned long long b) override { push("mark", m, ts, b); }
    void on_word_position(unsigned p, unsigned long long ts, unsigned long long b) override { push("wordpos", p, ts, b); }
    void on_done(unsigned long long ts, unsigned flags) override { push("done", flags, ts); }
    void on_audio_start(unsigned long long ts) override { push("audiostart", 0, ts); }
    void on_audio_stop(unsigned long long ts) override { push("audiostop", 0, ts); }
    void push(const char* k, unsigned id, unsigned long long ts, unsigned long long b = ~0ull) {
        std::lock_guard<std::mutex> lock(mtx);
        events.push_back({ k, id, ts, b == ~0ull ? pcm.size() : b, GetTickCount() });
    }
};

static bool write_wav(const std::string& path, const WAVEFORMATEX& fmt, const std::vector<BYTE>& pcm) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    DWORD data_len = static_cast<DWORD>(pcm.size());
    DWORD riff_len = 36 + data_len;
    fwrite("RIFF", 1, 4, f); fwrite(&riff_len, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); DWORD fmt_len = 16; fwrite(&fmt_len, 4, 1, f);
    WORD tag = 1; fwrite(&tag, 2, 1, f);
    fwrite(&fmt.nChannels, 2, 1, f); fwrite(&fmt.nSamplesPerSec, 4, 1, f);
    fwrite(&fmt.nAvgBytesPerSec, 4, 1, f); fwrite(&fmt.nBlockAlign, 2, 1, f); fwrite(&fmt.wBitsPerSample, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_len, 4, 1, f);
    fwrite(pcm.data(), 1, pcm.size(), f);
    fclose(f);
    return true;
}

struct Probe {
    stl::Engine eng;
    Collector col;
    std::string outdir;
    std::wstring eng_dir;

    void dump_state(const char* label) {
        stl::EngineState st;
        eng.get_state(st);
        rep("STATE[%s]: gender=%d pitch=%d speed=%d volume=%d bright=%d number=%d modes=[%d %d %d %d %d %d %d] version=%d.%d.%d",
            label, st.gender, st.pitch, st.speed, st.volume, st.bright, st.number_samples,
            st.modes[0], st.modes[1], st.modes[2], st.modes[3], st.modes[4], st.modes[5], st.modes[6],
            st.version[0], st.version[1], st.version[2]);
    }

    size_t speak(const std::string& name, const std::string& text, bool save = true, DWORD timeout = 60000) {
        if (!eng.running()) {
            std::string err;
            rep("engine not running before [%s]; restarting: %s", name.c_str(), eng.start(eng_dir, &err) ? "ok" : err.c_str());
            g_engine_pid = eng.engine_pid();
        }
        col.reset();
        DWORD t0 = GetTickCount();
        HRESULT hr = eng.speak(text, col, timeout);
        DWORD t1 = GetTickCount();
        WAVEFORMATEX fmt = eng.wave_format();
        size_t bytes = col.pcm.size();
        double secs = fmt.nAvgBytesPerSec ? bytes / double(fmt.nAvgBytesPerSec) : 0;
        rep("SPEAK[%s]: hr=0x%08lX total=%lums firstAudio=+%lums bytes=%u (%.2fs audio, rtf=%.1fx) chunks=%u min=%u max=%u",
            name.c_str(), hr, t1 - t0, col.first_tick ? col.first_tick - t0 : 0, (unsigned)bytes, secs,
            (t1 - t0) > 0 ? secs * 1000.0 / (t1 - t0) : 0.0, col.chunks, col.chunks ? col.min_chunk : 0, col.max_chunk);
        for (auto& e : col.events)
            rep("   %-10s id=%-6u ts=%-8llu bytesAt=%-8llu (%.3fs) t=+%lums", e.kind.c_str(), e.id, e.ts, e.bytes,
                fmt.nAvgBytesPerSec ? e.bytes / double(fmt.nAvgBytesPerSec) : 0.0, e.tick - t0);
        if (save && bytes > 0) {
            std::string path = outdir + "\\" + name + ".wav";
            write_wav(path, fmt, col.pcm);
            rep("   wrote %s", path.c_str());
        }
        return bytes;
    }
};

static void probe_ranges(Probe& p) {
    stl::Engine& e = p.eng;
    rep("=== RANGE PROBING ===");
    rep("--- pitch (male) ---");
    for (int v : { 50, 69, 70, 71, 90, 100, 120, 149, 150, 151, 200, 300 })
        rep("set_pitch(%d) -> 0x%08lX, get_pitch -> %d", v, e.set_pitch(v), e.get_pitch());
    e.set_pitch(stl::PITCH_MALE_DEFAULT);
    rep("--- speed ---");
    for (int v : { 50, 99, 100, 101, 150, 200, 250, 299, 300, 301, 400, 1000 })
        rep("set_speed(%d) -> 0x%08lX, get_speed -> %d", v, e.set_speed(v), e.get_speed());
    e.set_speed(stl::SPEED_DEFAULT);
    rep("--- volume (0..255) ---");
    for (int v : { 0, 1, 64, 128, 200, 254, 255 })
        rep("set_volume(%d) -> 0x%08lX, get_volume -> %d", v, e.set_volume(v), e.get_volume());
    e.set_volume(255);
    rep("--- brightness (0..15) ---");
    for (int v : { 0, 1, 5, 8, 10, 11, 15 })
        rep("set_bright(%d) -> 0x%08lX, get_bright -> %d", v, e.set_bright(v), e.get_bright());
    e.set_bright(stl::BRIGHT_MALE_DEFAULT);
    rep("--- gender ---");
    for (int g : { 1, 2, 1, 2 }) {
        HRESULT hr = e.set_gender(g);
        rep("set_gender(%d) -> 0x%08lX, get_gender -> %d, pitch=%d speed=%d bright=%d loaded(m,f,n)=%d,%d,%d",
            g, hr, e.get_gender(), e.get_pitch(), e.get_speed(), e.get_bright(),
            e.is_loaded(0), e.is_loaded(1), e.is_loaded(2));
    }
    rep("--- pitch (female) ---");
    e.set_gender(stl::GENDER_FEMALE);
    for (int v : { 100, 144, 145, 146, 195, 250, 299, 300, 301, 400 })
        rep("set_pitch(%d) -> 0x%08lX, get_pitch -> %d", v, e.set_pitch(v), e.get_pitch());
    e.set_gender(stl::GENDER_MALE);
    rep("--- number samples ---");
    for (int v : { 1, 0 })
        rep("set_number_samples(%d) -> 0x%08lX, get -> %d, loaded=%d", v, e.set_number_samples(v), e.get_number_samples(), e.is_loaded(2));
    rep("--- modes ---");
    for (int i = 1; i <= 6; i++)
        for (int v : { 1, 0 })
            rep("set_mode(%d,%d) -> 0x%08lX, get_mode -> %d", i, v, e.set_mode(i, v), e.get_mode(i));
    rep("set_mode(7,1) -> 0x%08lX (expect 0x80070057)", e.set_mode(7, 1));
    rep("set_mode(0,0) reset -> 0x%08lX, get_mode(0) -> %d", e.set_mode(0, 0), e.get_mode(0));
    p.dump_state("after ranges");
}

static void probe_abort(Probe& p) {
    stl::Engine& e = p.eng;
    rep("=== CANCEL TESTS ===");
    std::string longtext;
    for (int i = 0; i < 12; i++) longtext += "This is sentence number " + std::to_string(i + 1) + " of a long paragraph used to test cancelling speech in the middle. ";
    // cancel from the sink after ~1 second of audio
    p.col.reset();
    p.col.cancel_after_bytes_enabled = true;
    p.col.cancel_after_bytes = stl::SAMPLE_RATE * 2;   // 1 s of 16-bit audio
    DWORD t0 = GetTickCount();
    HRESULT hr = e.speak(longtext, p.col, 60000);
    rep("cancel-from-sink: hr=0x%08lX total=%lums bytes=%u (%.2fs) chunks=%u", hr, GetTickCount() - t0,
        (unsigned)p.col.pcm.size(), p.col.pcm.size() / double(stl::SAMPLE_RATE * 2), p.col.chunks);
    for (auto& ev : p.col.events) rep("   %-10s id=%u bytesAt=%llu t=+%lums", ev.kind.c_str(), ev.id, ev.bytes, ev.tick - t0);
    size_t b = p.speak("70_after_cancel", "Speech works again after cancelling.");
    rep("post-cancel utterance bytes=%u", (unsigned)b);

    // cancel from another thread after 600 ms
    p.col.reset();
    struct Ctx { stl::Engine* e; } ctx{ &e };
    HANDLE th = CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD { Sleep(600); static_cast<Ctx*>(lp)->e->abort(); return 0; }, &ctx, 0, nullptr);
    t0 = GetTickCount();
    hr = e.speak(longtext, p.col, 60000);
    DWORD t1 = GetTickCount();
    WaitForSingleObject(th, 2000); CloseHandle(th);
    rep("cancel-from-thread: hr=0x%08lX total=%lums bytes=%u (%.2fs) chunks=%u", hr, t1 - t0,
        (unsigned)p.col.pcm.size(), p.col.pcm.size() / double(stl::SAMPLE_RATE * 2), p.col.chunks);
    for (auto& ev : p.col.events) rep("   %-10s id=%u bytesAt=%llu t=+%lums", ev.kind.c_str(), ev.id, ev.bytes, ev.tick - t0);
    b = p.speak("71_after_thread_cancel", "And it still works after a cancel from another thread.");
    rep("post-cancel utterance bytes=%u", (unsigned)b);
    // cancel while idle must be harmless
    e.abort();
    b = p.speak("72_after_idle_cancel", "Cancelling while idle did no harm.");
}


static int run_acktest(Probe& p) {
    stl::Engine& e = p.eng;
    e.set_verbose(true);
    rep("=== ACK DIAGNOSTICS ===");
    rep("idle: sentinel pending before anything? %s", e.debug_take_sentinel() ? "YES" : "no");
    for (int v : { 100, 120, 80 }) {
        HRESULT hr = e.set_pitch(v);
        Sleep(100);
        bool extra = e.debug_take_sentinel();
        rep("set_pitch(%d) -> 0x%08lX; extra ack pending after 100ms: %s", v, hr, extra ? "YES" : "no");
        int g = e.get_pitch();
        Sleep(100);
        extra = e.debug_take_sentinel();
        rep("get_pitch -> %d; extra ack pending after 100ms: %s", g, extra ? "YES" : "no");
    }
    for (int v : { 1, 0 }) {
        HRESULT hr = e.set_mode(4, v);
        Sleep(100);
        bool extra = e.debug_take_sentinel();
        rep("set_mode(4,%d) -> 0x%08lX; extra ack: %s", v, hr, extra ? "YES" : "no");
        int g = e.get_mode(4);
        Sleep(100);
        extra = e.debug_take_sentinel();
        rep("get_mode(4) -> %d; extra ack: %s", g, extra ? "YES" : "no");
    }
    rep("--- speak and watch for stray acknowledgements ---");
    unsigned long long cpu0 = e.engine_cpu_ms();
    DWORD t0 = GetTickCount();
    p.speak("acktest_utterance", "This utterance is about four seconds long so that the playback simulation can be observed carefully.", true, 60000);
    DWORD t1 = GetTickCount();
    for (int i = 0; i < 80; i++) {
        Sleep(100);
        if (e.debug_take_sentinel()) rep("stray ack at +%lums after speak returned", GetTickCount() - t1);
    }
    unsigned long long cpu1 = e.engine_cpu_ms();
    rep("engine CPU during utterance+8s: %llu ms of %lu ms wall", cpu1 - cpu0, GetTickCount() - t0);
    for (int v : { 110, 95 }) {
        HRESULT hr = e.set_pitch(v);
        int g = e.get_pitch();
        rep("after speak: set_pitch(%d) -> 0x%08lX, get_pitch -> %d", v, hr, g);
    }
    rep("--- speak, then set/get while the playback simulation is still running ---");
    p.speak("acktest_utterance2", "A second utterance that keeps the engine in its playback simulation for a few seconds.", false, 60000);
    for (int v : { 100, 130 }) {
        HRESULT hr = e.set_pitch(v);
        int g = e.get_pitch();
        rep("during playback: set_pitch(%d) -> 0x%08lX, get_pitch -> %d", v, hr, g);
    }
    Sleep(6000);
    rep("--- japanese mode crash isolation ---");
    const char* jp[] = { "1234", "The number 1234.", "12345", "123456", "1234567", "12345678", "The population is 12345678 people, and 1997." };
    for (const char* t : jp) {
        e.set_mode(stl::MODE_JAPANESE, 1);
        size_t b = p.speak(std::string("jp_") + std::to_string(strlen(t)), t, true, 30000);
        bool alive = e.running();
        rep("japanese mode text \"%s\": bytes=%u engine alive=%s", t, (unsigned)b, alive ? "yes" : "NO");
        if (!alive) {
            std::string err;
            rep("restarting engine: %s", e.start(p.eng_dir, &err) ? "ok" : err.c_str());
            g_engine_pid = e.engine_pid();
        }
        Sleep(200);
    }
    e.set_mode(stl::MODE_JAPANESE, 0);
    e.set_mode(stl::MODE_RESET, 0);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: cybertalk_probe <engine dir> <output dir> [quick]\n");
        return 2;
    }
    std::string engine_dir = argv[1], outdir = argv[2];
    bool quick = argc > 3 && strcmp(argv[3], "quick") == 0;
    bool acktest = argc > 3 && strcmp(argv[3], "acktest") == 0;
    bool from_modes = argc > 3 && strcmp(argv[3], "modes") == 0;
    CreateDirectoryA(outdir.c_str(), nullptr);
    g_report = fopen((outdir + "\\probe_report.txt").c_str(), "w");
    HANDLE wd = CreateThread(nullptr, 0, watchdog_thread, nullptr, 0, nullptr);

    Probe p;
    p.outdir = outdir;
    p.eng.set_logger([](const char* s) { rep("  [engine] %s", s); });
    p.eng.set_verbose(true);
    std::wstring wdir(engine_dir.begin(), engine_dir.end());
    p.eng_dir = wdir;
    DWORD t0 = GetTickCount();
    std::string err;
    if (!p.eng.start(wdir, &err)) {
        rep("INIT FAILED: %s", err.c_str());
        g_watchdog_stop = true;
        return 1;
    }
    g_engine_pid = p.eng.engine_pid();
    rep("init took %lums, engine pid %lu", GetTickCount() - t0, g_engine_pid.load());
    p.dump_state("initial");
    if (acktest) {
        run_acktest(p);
        p.eng.stop();
        g_watchdog_stop = true;
        WaitForSingleObject(wd, 2000);
        fclose(g_report);
        return 0;
    }
    if (from_modes) goto modes_tests;

    rep("=== BASIC SPEECH ===");
    p.speak("01_male_default", "Hello. This is the Panasonic CyberTalk text to speech engine, speaking with the default male voice.");
    p.eng.set_verbose(false);
    p.speak("02_male_marks", "\\Mrk=1\\One \\Mrk=2\\two \\Mrk=3\\three \\Mrk=4\\four \\Mrk=5\\five.");
    p.speak("03_male_numbers", "The year 1997. Call 555-1234. Room 101. The total is $19.99 for 42 items, or 3.5 percent.");
    p.speak("04_male_punctuation", "Hello, world! Does this work? Yes: it does; mostly. \"Quoted text\" and (parentheses) and a hyphen-word.");
    p.speak("05_male_backslash", "A literal backslash \\\\ and a path C:\\\\Windows\\\\System32.");
    p.speak("06_unicode_translit", stl::utf8_to_engine_text("Caf\xC3\xA9 na\xC3\xAFve r\xC3\xA9sum\xC3\xA9 \xE2\x80\x9Csmart quotes\xE2\x80\x9D and an em\xE2\x80\x94""dash, 25\xC2\xB0 outside."));

    probe_ranges(p);

    rep("=== PARAMETER SWEEPS ===");
    stl::Engine& e = p.eng;
    e.set_gender(stl::GENDER_MALE);
    e.set_speed(200); e.set_volume(255); e.set_bright(8);
    for (int v : { 70, 90, 110, 130, 150 }) {
        e.set_pitch(v);
        p.speak("10_male_pitch_" + std::to_string(v), "This is the male voice at pitch " + std::to_string(v) + " hertz.");
    }
    e.set_pitch(90);
    for (int v : { 100, 150, 200, 250, 300 }) {
        e.set_speed(v);
        p.speak("11_speed_" + std::to_string(v), "This is the male voice speaking at " + std::to_string(v) + " words per minute, the quick brown fox jumps over the lazy dog.");
    }
    e.set_speed(200);
    for (int v : { 32, 96, 160, 255 }) {
        e.set_volume(v);
        p.speak("12_volume_" + std::to_string(v * 100 / 255), "This is the male voice at " + std::to_string(v * 100 / 255) + " percent volume.");
    }
    e.set_volume(255);
    for (int v : { 0, 4, 8, 12, 15 }) {
        e.set_bright(v);
        p.speak("13_bright_" + std::to_string(v), "This is the male voice with brightness " + std::to_string(v) + ".");
    }
    e.set_bright(8);

    rep("--- female voice ---");
    e.set_gender(stl::GENDER_FEMALE);
    p.dump_state("female");
    p.speak("20_female_default", "Hello. This is the Panasonic CyberTalk text to speech engine, speaking with the default female voice.");
    for (int v : { 145, 175, 195, 250, 300 }) {
        e.set_pitch(v);
        p.speak("21_female_pitch_" + std::to_string(v), "This is the female voice at pitch " + std::to_string(v) + " hertz.");
    }
    e.set_pitch(195);
    for (int v : { 0, 6, 12 }) {
        e.set_bright(v);
        p.speak("22_female_bright_" + std::to_string(v), "This is the female voice with brightness " + std::to_string(v) + ".");
    }
    e.set_bright(6);
    for (int v : { 100, 300 }) {
        e.set_speed(v);
        p.speak("23_female_speed_" + std::to_string(v), "This is the female voice at " + std::to_string(v) + " words per minute.");
    }
    e.set_speed(200);
    e.set_gender(stl::GENDER_MALE);

modes_tests:
    rep("--- reading modes ---");
    const std::string numtext = "Call 555-1234, extension 90210. Order 42 items at $19.99 each, total 839.58. The code is 0 0 7 and the year 2001.";
    e.set_mode(stl::MODE_RESET, 0);
    p.speak("30_modes_default", numtext);
    e.set_number_samples(1);
    p.speak("31_number_samples_on", numtext);
    e.set_number_samples(0);
    e.set_mode(stl::MODE_ZERO, 1);
    p.speak("32_zero_mode_on", "The code is 0 0 7, room 101, and 2001.");
    e.set_mode(stl::MODE_ZERO, 0);
    p.speak("32_zero_mode_off", "The code is 0 0 7, room 101, and 2001.");
    e.set_mode(stl::MODE_DOLLAR, 0);
    p.speak("33_dollar_mode_off", "The price is $19.99 and $5.");
    e.set_mode(stl::MODE_DOLLAR, 1);
    p.speak("33_dollar_mode_on", "The price is $19.99 and $5.");
    e.set_mode(stl::MODE_JAPANESE, 1);
    p.speak("34_japanese_mode_on", "The population is 12345678 people, and 1997.");
    e.set_mode(stl::MODE_JAPANESE, 0);
    p.speak("34_japanese_mode_off", "The population is 12345678 people, and 1997.");
    const std::string lines = "Name\tAge\tCity\nAlice\t30\tBoston\nBob\t25\tDenver\n";
    e.set_mode(stl::MODE_SPREADSHEET, 1);
    p.speak("35_spreadsheet_mode", lines);
    e.set_mode(stl::MODE_SPREADSHEET, 0);
    e.set_mode(stl::MODE_LIST, 1);
    p.speak("36_list_mode", "apples\noranges\npears\nbananas\n");
    e.set_mode(stl::MODE_LIST, 0);
    p.speak("37_plain_lines", "apples\noranges\npears\nbananas\n");
    e.set_mode(stl::MODE_RESET, 0);

    rep("--- inline tags ---");
    p.speak("40_tag_emphasis", "This word is \\Emp\\important. This one is \\Emp=NOSTRESS\\unstressed, and \\Emp=EMPHASIZE\\shouted.");
    p.speak("41_tag_pause", "Before the pause. \\Pau=1500\\ After a one and a half second pause.");
    p.speak("42_tag_pitch_speed", "Normal pitch. \\Pit=130\\ High pitch. \\Pit=75\\ Low pitch. \\Rst\\ \\Spd=300\\ Very fast speech now. \\Spd=100\\ Very slow speech now. \\Rst\\ Back to normal.");
    p.dump_state("after pitch/speed tags");
    p.speak("43_tag_voice_gender", "This is the male voice. \\Vce=Gender=Female\\ Now this is the female voice. \\Vce=Gender=Male\\ And male again.");
    p.dump_state("after gender tag");
    p.speak("44_tag_volume", "Full volume. \\Vol=16000\\ Quarter volume. \\Vol=65535\\ Full again.");
    p.dump_state("after volume tag");
    p.speak("45_tag_context", "\\Ctx=E-Mail\\ Contact john.smith@example.com or visit www.panasonic.com. \\Ctx=Address\\ 1600 Pennsylvania Ave NW, Washington DC 20500.");
    p.speak("46_tag_partofspeech", "I \\Prt=V\\ read the book yesterday. Please \\Prt=N\\ record the record.");
    p.speak("47_tag_pronounce", "\\Eng:PRN=cybertalk=S AY B ER T AO K,PRT=N\\ CyberTalk is the name of this engine.");
    p.speak("48_tag_phonemes", "\\Eng:PHO=HH AH L OW W ER L D\\ was spoken from phonemes.");
    p.speak("49_tag_set_number", "\\Eng:SET:NUMBER\\ 1 2 3 4 5 6 7 8 9 10, 42 and 1997. \\Eng:RST:NUMBER\\ 42 and 1997 again.");
    p.speak("50_spelling", "N V D A. J A W S. A B C D E F G.");
    p.speak("51_unknown_tag", "An unknown tag \\Foo=1\\ should be ignored, and so should \\Rst\\ this reset.");
    e.set_mode(stl::MODE_RESET, 0);
    e.set_gender(stl::GENDER_MALE);
    e.set_speed(200); e.set_pitch(90); e.set_volume(255); e.set_bright(8);

    if (!quick) probe_abort(p);

    rep("=== TIMING ===");
    p.speak("60_short", "Hi.");
    p.speak("61_short", "OK");
    p.speak("62_long", "The Speech Technology Laboratory was a research division of Panasonic in Santa Barbara, California. CyberTalk was its text to speech product for Windows 95, released in 1997 with a male and a female voice. This paragraph exists to measure how fast the engine synthesises a longer passage of text, and to give a reasonable sample of connected speech.");
    p.speak("63_empty", "");
    p.speak("64_spaces", "   ");
    p.speak("65_long_word_marks", "\\Mrk=1\\The \\Mrk=2\\quick \\Mrk=3\\brown \\Mrk=4\\fox \\Mrk=5\\jumps \\Mrk=6\\over \\Mrk=7\\the \\Mrk=8\\lazy \\Mrk=9\\dog, \\Mrk=10\\and \\Mrk=11\\then \\Mrk=12\\it \\Mrk=13\\sleeps.");
    p.dump_state("final");

    p.eng.stop();
    Sleep(300);
    rep("engine process still running after stop: %s", p.eng.running() ? "YES" : "no");
    g_watchdog_stop = true;
    WaitForSingleObject(wd, 2000);
    rep("PROBE COMPLETE");
    fclose(g_report);
    return 0;
}
