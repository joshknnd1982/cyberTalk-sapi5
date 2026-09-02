#include "pipe_client.h"
#include "ct_log.h"
#include <shlwapi.h>
#include <cstring>

#pragma comment(lib, "shlwapi.lib")

namespace {

HMODULE this_module() {
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&this_module), &h);
    return h;
}

bool file_exists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

PipeClient::PipeClient() = default;
PipeClient::~PipeClient() { disconnect(); }

std::wstring PipeClient::find_host_exe() {
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(this_module(), path, MAX_PATH)) {
        PathRemoveFileSpecW(path);
        std::wstring dir = path;
        std::wstring cand = dir + L"\\CyberTalkHost.exe";
        if (file_exists(cand)) return cand;
        PathRemoveFileSpecW(path);
        cand = std::wstring(path) + L"\\CyberTalkHost.exe";
        if (file_exists(cand)) return cand;
    }
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\CyberTalkSAPI", 0, KEY_READ | KEY_WOW64_32KEY, &k) == ERROR_SUCCESS) {
        wchar_t dir[MAX_PATH] = {};
        DWORD size = sizeof(dir);
        DWORD type = 0;
        if (RegQueryValueExW(k, L"InstallDir", nullptr, &type, reinterpret_cast<LPBYTE>(dir), &size) == ERROR_SUCCESS && type == REG_SZ) {
            std::wstring cand = dir;
            if (!cand.empty() && cand.back() != L'\\') cand += L'\\';
            cand += L"CyberTalkHost.exe";
            RegCloseKey(k);
            if (file_exists(cand)) return cand;
        } else {
            RegCloseKey(k);
        }
    }
    return L"";
}

bool PipeClient::open_pipe() {
    for (int attempt = 0; attempt < 3; attempt++) {
        HANDLE h = CreateFileW(CYBERTALK_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_BYTE;
            SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
            pipe_ = h;
            return true;
        }
        DWORD e = GetLastError();
        if (e == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(CYBERTALK_PIPE_NAME, 2000);
            continue;
        }
        return false;
    }
    return false;
}

bool PipeClient::launch_host() {
    HANDLE launch_mutex = CreateMutexW(nullptr, FALSE, CYBERTALK_LAUNCH_MUTEX);
    if (launch_mutex) WaitForSingleObject(launch_mutex, 10000);
    bool ok = false;
    // another client may have launched it while we waited
    HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, CYBERTALK_HOST_MUTEX);
    if (running) {
        CloseHandle(running);
        ok = true;
    } else {
        std::wstring exe = find_host_exe();
        if (exe.empty()) {
            err_ = "CyberTalkHost.exe not found";
            CT_LOG("pipe: %s", err_.c_str());
        } else {
            STARTUPINFOW si = { sizeof(si) };
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi = {};
            std::wstring cmdline = L"\"" + exe + L"\"";
            std::vector<wchar_t> cl(cmdline.begin(), cmdline.end());
            cl.push_back(0);
            std::wstring dir = exe;
            size_t slash = dir.find_last_of(L'\\');
            if (slash != std::wstring::npos) dir.resize(slash);
            if (CreateProcessW(exe.c_str(), cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS,
                               nullptr, dir.c_str(), &si, &pi)) {
                CT_LOGW(L"pipe: launched host %s (pid %lu)", exe.c_str(), pi.dwProcessId);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                ok = true;
            } else {
                char b[128];
                snprintf(b, sizeof(b), "CreateProcess(CyberTalkHost.exe) failed (%lu)", GetLastError());
                err_ = b;
                CT_LOG("pipe: %s", err_.c_str());
            }
        }
    }
    if (launch_mutex) {
        ReleaseMutex(launch_mutex);
        CloseHandle(launch_mutex);
    }
    return ok;
}

bool PipeClient::connect() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pipe_ != INVALID_HANDLE_VALUE) return true;
    if (open_pipe()) return true;
    if (!launch_host()) return false;
    // the host needs a moment to bring the engine up
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < 25000) {
        if (open_pipe()) {
            CT_LOG("pipe: connected after %lu ms", GetTickCount() - t0);
            return true;
        }
        Sleep(100);
    }
    err_ = "timed out waiting for the CyberTalk host pipe";
    CT_LOG("pipe: %s", err_.c_str());
    return false;
}

void PipeClient::disconnect() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

bool PipeClient::write_all(const void* p, uint32_t n) {
    const char* b = static_cast<const char*>(p);
    while (n > 0) {
        DWORD w = 0;
        if (!WriteFile(pipe_, b, n, &w, nullptr) || w == 0) {
            char e[96];
            snprintf(e, sizeof(e), "pipe write failed (%lu)", GetLastError());
            err_ = e;
            return false;
        }
        b += w;
        n -= w;
    }
    return true;
}

bool PipeClient::read_all(void* p, uint32_t n) {
    char* b = static_cast<char*>(p);
    while (n > 0) {
        DWORD r = 0;
        if (!ReadFile(pipe_, b, n, &r, nullptr) || r == 0) {
            char e[96];
            snprintf(e, sizeof(e), "pipe read failed (%lu)", GetLastError());
            err_ = e;
            return false;
        }
        b += r;
        n -= r;
    }
    return true;
}

bool PipeClient::send(uint32_t type, const void* payload, uint32_t size) {
    PipeMessageHeader h = { type, size };
    if (!write_all(&h, sizeof(h))) return false;
    if (size && !write_all(payload, size)) return false;
    return true;
}

bool PipeClient::recv(PipeMessageHeader& h, std::vector<char>& payload) {
    if (!read_all(&h, sizeof(h))) return false;
    payload.resize(h.size);
    if (h.size && !read_all(payload.data(), h.size)) return false;
    return true;
}

bool PipeClient::ping() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pipe_ == INVALID_HANDLE_VALUE) return false;
    if (!send(CMD_PING, nullptr, 0)) { disconnect(); return false; }
    PipeMessageHeader h;
    std::vector<char> p;
    if (!recv(h, p)) { disconnect(); return false; }
    return h.type == RESP_PONG;
}

bool PipeClient::get_info(InfoResponse& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pipe_ == INVALID_HANDLE_VALUE) return false;
    if (!send(CMD_GET_INFO, nullptr, 0)) { disconnect(); return false; }
    PipeMessageHeader h;
    std::vector<char> p;
    if (!recv(h, p)) { disconnect(); return false; }
    if (h.type != RESP_INFO || p.size() < sizeof(InfoResponse)) return false;
    memcpy(&info, p.data(), sizeof(InfoResponse));
    return true;
}

bool PipeClient::get_state(StateResponse& st) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pipe_ == INVALID_HANDLE_VALUE) return false;
    if (!send(CMD_GET_STATE, nullptr, 0)) { disconnect(); return false; }
    PipeMessageHeader h;
    std::vector<char> p;
    if (!recv(h, p)) { disconnect(); return false; }
    if (h.type != RESP_STATE || p.size() < sizeof(StateResponse)) return false;
    memcpy(&st, p.data(), sizeof(StateResponse));
    return true;
}

bool PipeClient::speak(const SpeakCommand& cmd_in, const std::string& text, const SpeakEvents& ev, SpeakEndResponse& end) {
    std::lock_guard<std::mutex> lock(mutex_);
    end = SpeakEndResponse{};
    end.status = 2;
    if (pipe_ == INVALID_HANDLE_VALUE) { err_ = "not connected"; return false; }

    SpeakCommand cmd = cmd_in;
    cmd.text_length = static_cast<uint32_t>(text.size());
    std::vector<char> payload(sizeof(SpeakCommand) + text.size());
    memcpy(payload.data(), &cmd, sizeof(cmd));
    memcpy(payload.data() + sizeof(cmd), text.data(), text.size());
    if (!send(CMD_SPEAK, payload.data(), static_cast<uint32_t>(payload.size()))) { disconnect(); return false; }

    bool stop_sent = false;
    PipeMessageHeader h;
    std::vector<char> p;
    for (;;) {
        if (!recv(h, p)) { disconnect(); return false; }
        switch (h.type) {
        case RESP_AUDIO:
            if (!stop_sent && ev.audio && !ev.audio(p.data(), static_cast<uint32_t>(p.size()))) {
                stop_sent = true;
                if (!send(CMD_STOP, nullptr, 0)) { disconnect(); return false; }
            }
            break;
        case RESP_MARK:
            if (!stop_sent && ev.mark && p.size() >= sizeof(MarkResponse)) {
                MarkResponse m;
                memcpy(&m, p.data(), sizeof(m));
                ev.mark(m.id, m.byte_offset);
            }
            break;
        case RESP_WORD:
            if (!stop_sent && ev.word && p.size() >= sizeof(MarkResponse)) {
                MarkResponse m;
                memcpy(&m, p.data(), sizeof(m));
                ev.word(m.id, m.byte_offset);
            }
            break;
        case RESP_SPEAK_END:
            if (p.size() >= sizeof(SpeakEndResponse)) memcpy(&end, p.data(), sizeof(end));
            if (stop_sent && end.status == 0) end.status = 1;
            return true;
        case RESP_ERROR:
            if (p.size() >= sizeof(ErrorResponse)) {
                ErrorResponse er;
                memcpy(&er, p.data(), sizeof(er));
                er.message[sizeof(er.message) - 1] = 0;
                err_ = er.message;
                end.hresult = er.hresult;
            }
            end.status = 2;
            return true;
        case RESP_OK:
            break;   // acknowledgement of CMD_STOP
        default:
            break;
        }
    }
}

void PipeClient::shutdown_host() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pipe_ == INVALID_HANDLE_VALUE && !open_pipe()) return;
    send(CMD_SHUTDOWN, nullptr, 0);
    PipeMessageHeader h;
    std::vector<char> p;
    recv(h, p);
    disconnect();
}
