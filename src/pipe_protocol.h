// Named-pipe protocol between the CyberTalk host process (CyberTalkHost.exe,
// which owns the engine) and its clients: the x86 and x64 SAPI 5 engine DLLs
// and the configuration utility.
//
// Framing: every message is a PipeMessageHeader followed by `size` bytes of
// payload.  The client sends one command at a time and reads responses until
// the terminating response for that command arrives.  During CMD_SPEAK the
// host streams RESP_AUDIO / RESP_MARK / RESP_WORD messages and finishes with
// RESP_SPEAK_END.  A client may send CMD_STOP on the same pipe while a speak
// is in progress (the host reads it on a separate thread); it must keep
// reading responses until RESP_SPEAK_END arrives.
#pragma once

#include <stdint.h>

#define CYBERTALK_PIPE_NAME L"\\\\.\\pipe\\CyberTalkTTS"
#define CYBERTALK_HOST_MUTEX L"Local\\CyberTalkHostMutex"
#define CYBERTALK_LAUNCH_MUTEX L"Local\\CyberTalkHostLaunchMutex"
#define CYBERTALK_PROTOCOL_VERSION 3

enum PipeCommand : uint32_t {
    CMD_PING = 1,
    CMD_GET_INFO = 2,       // -> RESP_INFO
    CMD_SPEAK = 3,          // SpeakCommand + text -> RESP_AUDIO*/RESP_MARK*/RESP_WORD* ... RESP_SPEAK_END
    CMD_STOP = 4,           // -> RESP_OK (and the running speak ends with RESP_SPEAK_END)
    CMD_SHUTDOWN = 5,       // -> RESP_OK, host exits
    CMD_GET_STATE = 6,      // -> RESP_STATE (current engine parameters)
};

enum PipeResponse : uint32_t {
    RESP_OK = 100,
    RESP_ERROR = 101,       // ErrorResponse
    RESP_PONG = 102,
    RESP_INFO = 103,        // InfoResponse
    RESP_AUDIO = 104,       // raw PCM bytes (16-bit mono, InfoResponse.sample_rate)
    RESP_MARK = 105,        // MarkResponse
    RESP_WORD = 106,        // MarkResponse (id = word position reported by the engine)
    RESP_SPEAK_END = 107,   // SpeakEndResponse
    RESP_STATE = 108,       // StateResponse
};

#pragma pack(push, 1)
struct PipeMessageHeader {
    uint32_t type;
    uint32_t size;
};

// Reading-mode bit flags (SpeakCommand.modes)
enum SpeakModeFlags : uint32_t {
    MODE_FLAG_SPREADSHEET = 1u << 0,
    MODE_FLAG_LIST = 1u << 1,
    MODE_FLAG_DOLLAR = 1u << 2,           // default on
    MODE_FLAG_ZERO = 1u << 3,
    MODE_FLAG_NUMBER_SAMPLES = 1u << 4,
    MODE_FLAG_JAPANESE = 1u << 5,
};

struct SpeakCommand {
    uint32_t gender;          // 1 female, 2 male
    uint32_t pitch;           // Hz, engine range depends on gender
    uint32_t speed;           // words per minute 100..300
    uint32_t volume;          // 0..255
    uint32_t bright;          // 0..15
    uint32_t modes;           // SpeakModeFlags
    float sonic_speed;        // extra time-stretch applied by the host (1.0 = none)
    uint32_t flags;           // reserved, 0
    uint32_t text_length;     // bytes of engine text (7-bit ASCII with \Tag\ codes) following
};

struct MarkResponse {
    uint32_t id;
    uint64_t byte_offset;     // audio bytes delivered before the mark (after sonic)
};

struct SpeakEndResponse {
    uint32_t status;          // 0 completed, 1 cancelled, 2 error
    uint32_t hresult;
    uint64_t total_bytes;
};

struct ErrorResponse {
    uint32_t hresult;
    char message[256];
};

struct InfoResponse {
    uint32_t protocol_version;
    uint32_t sample_rate;
    uint32_t bits_per_sample;
    uint32_t channels;
    uint32_t engine_version[3];
    uint32_t speed_min, speed_max, speed_default;
    uint32_t pitch_male_min, pitch_male_max, pitch_male_default;
    uint32_t pitch_female_min, pitch_female_max, pitch_female_default;
    uint32_t volume_max, volume_default;
    uint32_t bright_max, bright_male_default, bright_female_default;
    uint32_t host_pid;
    uint32_t engine_pid;
};

struct StateResponse {
    int32_t gender, pitch, speed, volume, bright, number_samples;
    int32_t modes[7];
};
#pragma pack(pop)
