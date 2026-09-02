// Client side of the CyberTalk host pipe protocol (see pipe_protocol.h).
// Used by the x86 and x64 SAPI 5 DLLs and by the configuration utility.
#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include "pipe_protocol.h"

class PipeClient {
public:
    PipeClient();
    ~PipeClient();
    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    // Connect to the host, launching CyberTalkHost.exe if it is not running.
    bool connect();
    void disconnect();
    bool connected() const { return pipe_ != INVALID_HANDLE_VALUE; }

    bool ping();
    bool get_info(InfoResponse& info);
    bool get_state(StateResponse& st);

    struct SpeakEvents {
        std::function<bool(const void* data, uint32_t size)> audio;     // return false to stop
        std::function<void(uint32_t id, uint64_t byte_offset)> mark;
        std::function<void(uint32_t id, uint64_t byte_offset)> word;
    };
    // Runs a complete speak exchange.  Returns false only when the pipe failed;
    // cancellation and engine errors are reported through `end`.
    bool speak(const SpeakCommand& cmd, const std::string& engine_text, const SpeakEvents& ev, SpeakEndResponse& end);

    // Ask the host to exit (used by uninstall / DllUnregisterServer).
    void shutdown_host();

    const std::string& last_error() const { return err_; }

    // Locate CyberTalkHost.exe next to this module, in its parent directory,
    // or through the installation registry key.
    static std::wstring find_host_exe();

private:
    bool write_all(const void* p, uint32_t n);
    bool read_all(void* p, uint32_t n);
    bool send(uint32_t type, const void* payload, uint32_t size);
    bool recv(PipeMessageHeader& h, std::vector<char>& payload);
    bool open_pipe();
    bool launch_host();

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::string err_;
    std::mutex mutex_;
};
