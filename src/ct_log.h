// Shared file logger for the CyberTalk SAPI 5 wrapper.
//
// Every component (host, SAPI DLLs, configuration utility, test tools) logs to
// %LOCALAPPDATA%\CyberTalkSAPI\logs\<component>.log.  Lines carry a timestamp
// with milliseconds plus process and thread ids so the host and client logs can
// be correlated.  The file is opened per write with _SH_DENYNO so several
// processes can append concurrently, and it rolls over to .old at 4 MB.
#pragma once

#include <windows.h>
#include <shlobj.h>
#include <share.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <mutex>

namespace ctlog {

class Logger {
public:
    static Logger& instance() {
        static Logger l;
        return l;
    }

    // name: file base name (e.g. L"host", L"sapi_x86").  Idempotent.
    void init(const wchar_t* name) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!path_.empty()) return;
        wchar_t base[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, base))) {
            GetTempPathW(MAX_PATH, base);
        }
        dir_ = std::wstring(base) + L"\\CyberTalkSAPI\\logs";
        CreateDirectoryW((std::wstring(base) + L"\\CyberTalkSAPI").c_str(), nullptr);
        CreateDirectoryW(dir_.c_str(), nullptr);
        path_ = dir_ + L"\\" + name + L".log";
        enabled_ = true;
    }

    void set_enabled(bool e) { enabled_ = e; }
    bool enabled() const { return enabled_; }
    const std::wstring& path() const { return path_; }
    const std::wstring& dir() const { return dir_; }

    void log(const char* fmt, ...) {
        if (!enabled_ || path_.empty()) return;
        char msg[4096];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        write_line(msg);
    }

    void logw(const wchar_t* fmt, ...) {
        if (!enabled_ || path_.empty()) return;
        wchar_t wmsg[4096];
        va_list ap;
        va_start(ap, fmt);
        _vsnwprintf(wmsg, 4096, fmt, ap);
        va_end(ap);
        wmsg[4095] = 0;
        char msg[8192];
        WideCharToMultiByte(CP_UTF8, 0, wmsg, -1, msg, sizeof(msg), nullptr, nullptr);
        write_line(msg);
    }

private:
    Logger() = default;

    void write_line(const char* msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        rollover_locked();
        FILE* f = _wfsopen(path_.c_str(), L"a", _SH_DENYNO);
        if (!f) return;
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu:%lu] %s\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                GetCurrentProcessId(), GetCurrentThreadId(), msg);
        fclose(f);
    }

    void rollover_locked() {
        if (++counter_ % 64 != 0) return;
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &fad) && fad.nFileSizeLow > 4u * 1024 * 1024) {
            std::wstring old = path_ + L".old";
            DeleteFileW(old.c_str());
            MoveFileW(path_.c_str(), old.c_str());
        }
    }

    std::mutex mutex_;
    std::wstring path_;
    std::wstring dir_;
    bool enabled_ = false;
    unsigned counter_ = 0;
};

}  // namespace ctlog

#define CT_LOG(...) ctlog::Logger::instance().log(__VA_ARGS__)
#define CT_LOGW(...) ctlog::Logger::instance().logw(__VA_ARGS__)
