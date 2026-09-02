// Direct client for the Panasonic / Speech Technology Laboratory "CyberTalk"
// text-to-speech engine process (STLTTS.EXE, 1997).
//
// The engine runs as a separate 32-bit process and talks to its client over a
// small shared-memory protocol that was recovered from TTSAPI.DLL:
//
//   * named file mappings  "stlttsmem" (control block), "audioBuf" (PCM),
//                          "spellingBuf", "phonemeBuf"
//   * named semaphores     "SentinelSemaphore" (command acknowledgements),
//                          "BufferSemaphore"   (released after a cancel)
//   * named events         "bufNotifyEvent"/"bufNotifyReturn" (text progress),
//                          "NotifyEvent"/"notifyReturn"       (audio start/stop),
//                          "audioObjectCall"/"audioObjectReturn" (audio
//                          delivery), "audioMeterEvent"
//   * commands are registered window messages ("TXTDTA", "PIT", "SPD", ...)
//     posted to the engine's main thread; per-utterance text travels in a
//     pagefile-backed mapping whose handle is duplicated into the engine.
//
// Nothing here depends on SAPI 4, COM, or the registry.  This class is used by
// the CyberTalk host process (which serves the SAPI 5 DLLs over a pipe) and by
// the test/sample tools.  It is thread-safe: speak() serialises callers,
// abort() may be called from any thread.
#pragma once

#include <windows.h>
#include <functional>
#include <string>
#include <memory>
#include "stl_text.h"

namespace stl {

// Engine limits and defaults recovered from STLTTS.EXE
constexpr int SPEED_MIN = 100;   // words per minute
constexpr int SPEED_MAX = 300;
constexpr int SPEED_DEFAULT = 200;
constexpr int PITCH_MALE_MIN = 70;   // Hz
constexpr int PITCH_MALE_MAX = 150;
constexpr int PITCH_MALE_DEFAULT = 90;
constexpr int PITCH_FEMALE_MIN = 145;
constexpr int PITCH_FEMALE_MAX = 300;
constexpr int PITCH_FEMALE_DEFAULT = 195;
constexpr int VOLUME_MIN = 0;
constexpr int VOLUME_MAX = 255;
constexpr int VOLUME_DEFAULT = 255;
constexpr int BRIGHT_MIN = 0;
constexpr int BRIGHT_MAX = 15;
constexpr int BRIGHT_MALE_DEFAULT = 8;
constexpr int BRIGHT_FEMALE_DEFAULT = 6;
constexpr int SAMPLE_RATE = 11025;
constexpr int BITS_PER_SAMPLE = 16;
constexpr int CHANNELS = 1;

enum Gender { GENDER_FEMALE = 1, GENDER_MALE = 2 };

// Reading modes (set_mode / get_mode index)
enum Mode {
    MODE_RESET = 0,          // set: restore all modes to defaults; get: 1 if all defaults
    MODE_SPREADSHEET = 1,    // spreadsheet/table context (tab/newline separated cells)
    MODE_LIST = 2,           // list context (one item per line)
    MODE_DOLLAR = 3,         // "$12.50" read as dollars and cents (default on)
    MODE_ZERO = 4,           // digit 0 read as "zero" instead of "oh"
    MODE_NUMBER_SAMPLES = 5, // numbers spoken from the recorded NSAMPLES.DLL
    MODE_JAPANESE = 6        // Japanese style grouping of digits by four
};

// Sample-set identifiers for is_loaded()
enum SampleSet { SAMPLES_MALE = 0, SAMPLES_FEMALE = 1, SAMPLES_NUMBER = 2 };

using LogFn = std::function<void(const char*)>;

// Receives everything the engine produces while speak() runs.  All callbacks
// run on internal worker threads while the engine is blocked waiting for the
// callback to return, so keep them short.
struct SpeakSink {
    virtual ~SpeakSink() = default;
    // PCM audio (16-bit mono, sample_rate()).  Return false to cancel speech.
    virtual bool on_audio(const void* data, unsigned size) = 0;
    virtual void on_started(unsigned long long timestamp) { (void)timestamp; }
    // \Mrk=N\ reached.  bytes_so_far = audio bytes already delivered.
    virtual void on_mark(unsigned mark, unsigned long long timestamp, unsigned long long bytes_so_far) {
        (void)mark; (void)timestamp; (void)bytes_so_far;
    }
    virtual void on_word_position(unsigned pos, unsigned long long timestamp, unsigned long long bytes_so_far) {
        (void)pos; (void)timestamp; (void)bytes_so_far;
    }
    virtual void on_done(unsigned long long timestamp, unsigned flags) { (void)timestamp; (void)flags; }
    virtual void on_audio_start(unsigned long long timestamp) { (void)timestamp; }
    virtual void on_audio_stop(unsigned long long timestamp) { (void)timestamp; }
};

struct EngineState {
    int gender = 0;      // GENDER_MALE / GENDER_FEMALE
    int pitch = 0;       // Hz
    int speed = 0;       // wpm
    int volume = 0;      // 0..255
    int bright = 0;      // 0..15
    int number_samples = 0;
    int modes[7] = {};   // by Mode index
    int version[3] = {};
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void set_logger(LogFn fn);
    void set_verbose(bool v);
    // The engine paces itself in real time after synthesising (a busy loop
    // driven by timeGetTime).  A scale > 1 makes its clock run that many times
    // faster so the wait collapses; 0 leaves the engine untouched.
    void set_clock_scale(unsigned scale);

    // Launch STLTTS.EXE from engine_dir and wait until it reports readiness.
    bool start(const std::wstring& engine_dir, std::string* error = nullptr);
    void stop();
    bool running();
    DWORD engine_pid() const;

    // Parameters.  Return the engine's HRESULT-style result (0 = ok,
    // 0x8000FFFF = value out of range, 0x80070057 = bad index, E_FAIL = dead).
    HRESULT set_gender(int gender);
    HRESULT set_pitch(int hz);
    HRESULT set_speed(int wpm);
    HRESULT set_volume(int v);           // 0..255
    HRESULT set_bright(int b);           // 0..15
    HRESULT set_number_samples(int on);  // 0/1
    HRESULT set_mode(int index, int value);
    HRESULT load_samples(int which);     // SampleSet
    HRESULT unload_samples(int which);
    int get_gender();
    int get_pitch();
    int get_speed();
    int get_volume();
    int get_bright();
    int get_number_samples();
    int get_mode(int index);
    int is_loaded(int which);
    bool get_version(int out[3]);
    bool get_state(EngineState& st);

    // Speak engine text (7-bit ASCII, may contain \Tag\ control codes).
    // Blocks until the utterance is complete or cancelled.  Returns S_OK,
    // S_FALSE when cancelled, or an error.
    HRESULT speak(const std::string& text, SpeakSink& sink, DWORD timeout_ms = 300000);
    // Cancel a running speak() from another thread.
    void abort();
    bool aborted() const;

    WAVEFORMATEX wave_format() const;
    bool wave_format_known() const;

    // diagnostics: consume one pending acknowledgement if there is one
    bool debug_take_sentinel();
    // diagnostics: engine process CPU time in milliseconds (user + kernel)
    unsigned long long engine_cpu_ms();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace stl
