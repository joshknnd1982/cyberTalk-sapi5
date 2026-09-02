// Settings shared by the SAPI 5 DLLs and the configuration utility.
//
// Stored in %APPDATA%\CyberTalkSAPI\settings.ini.  The "CyberTalk Custom
// Voice" SAPI token follows these values; the fixed Male / Female tokens use
// the engine defaults.  The DLL re-reads the file whenever its modification
// time changes, so the configuration utility takes effect without restarting
// the screen reader.
#pragma once

#include <windows.h>
#include <shlobj.h>
#include <string>
#include <cstdio>
#include <cstdlib>

namespace ctsettings {

struct CustomVoice {
    int gender = 2;          // 1 female, 2 male
    int pitch = 90;          // Hz (engine range depends on gender)
    int speed = 200;         // words per minute 100..300
    int volume = 255;        // 0..255
    int bright = 8;          // 0..15
    int dollar = 1;          // "$" amounts read as dollars/cents
    int zero = 0;            // digit 0 as "zero" (1) or "oh" (0)
    int number_samples = 0;  // numbers from recorded samples
    int spreadsheet = 0;     // spreadsheet/table context
    int list = 0;            // list context
};

struct Settings {
    CustomVoice custom;
    int rate_boost = 1;      // allow SAPI rates beyond the engine's 300 wpm via time compression
    int logging = 1;         // detailed logs in %LOCALAPPDATA%\CyberTalkSAPI\logs
    int word_events = 1;     // report word boundary events to SAPI
};

inline std::wstring settings_dir() {
    wchar_t base[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, base))) return L"";
    std::wstring dir = std::wstring(base) + L"\\CyberTalkSAPI";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

inline std::wstring settings_path() {
    std::wstring d = settings_dir();
    return d.empty() ? L"" : d + L"\\settings.ini";
}

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline int pitch_min(int gender) { return gender == 1 ? 145 : 70; }
inline int pitch_max(int gender) { return gender == 1 ? 300 : 150; }
inline int pitch_default(int gender) { return gender == 1 ? 195 : 90; }
inline int bright_default(int gender) { return gender == 1 ? 6 : 8; }

inline void sanitize(Settings& s) {
    s.custom.gender = (s.custom.gender == 1) ? 1 : 2;
    s.custom.pitch = clampi(s.custom.pitch, pitch_min(s.custom.gender), pitch_max(s.custom.gender));
    s.custom.speed = clampi(s.custom.speed, 100, 300);
    s.custom.volume = clampi(s.custom.volume, 0, 255);
    s.custom.bright = clampi(s.custom.bright, 0, 15);
    s.custom.dollar = s.custom.dollar ? 1 : 0;
    s.custom.zero = s.custom.zero ? 1 : 0;
    s.custom.number_samples = s.custom.number_samples ? 1 : 0;
    s.custom.spreadsheet = s.custom.spreadsheet ? 1 : 0;
    s.custom.list = s.custom.list ? 1 : 0;
    s.rate_boost = s.rate_boost ? 1 : 0;
    s.logging = s.logging ? 1 : 0;
    s.word_events = s.word_events ? 1 : 0;
}

inline bool load(Settings& s, FILETIME* mtime = nullptr) {
    std::wstring path = settings_path();
    if (path.empty()) return false;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        sanitize(s);
        return false;
    }
    if (mtime) *mtime = fad.ftLastWriteTime;
    auto geti = [&](const wchar_t* section, const wchar_t* key, int def) {
        return static_cast<int>(GetPrivateProfileIntW(section, key, def, path.c_str()));
    };
    Settings d;
    s.custom.gender = geti(L"CustomVoice", L"Gender", d.custom.gender);
    s.custom.pitch = geti(L"CustomVoice", L"Pitch", pitch_default(s.custom.gender == 1 ? 1 : 2));
    s.custom.speed = geti(L"CustomVoice", L"Speed", d.custom.speed);
    s.custom.volume = geti(L"CustomVoice", L"Volume", d.custom.volume);
    s.custom.bright = geti(L"CustomVoice", L"Brightness", bright_default(s.custom.gender == 1 ? 1 : 2));
    s.custom.dollar = geti(L"CustomVoice", L"DollarMode", d.custom.dollar);
    s.custom.zero = geti(L"CustomVoice", L"ZeroMode", d.custom.zero);
    s.custom.number_samples = geti(L"CustomVoice", L"NumberSamples", d.custom.number_samples);
    s.custom.spreadsheet = geti(L"CustomVoice", L"SpreadsheetMode", d.custom.spreadsheet);
    s.custom.list = geti(L"CustomVoice", L"ListMode", d.custom.list);
    s.rate_boost = geti(L"General", L"RateBoost", d.rate_boost);
    s.logging = geti(L"General", L"Logging", d.logging);
    s.word_events = geti(L"General", L"WordEvents", d.word_events);
    sanitize(s);
    return true;
}

inline bool save(const Settings& in) {
    Settings s = in;
    sanitize(s);
    std::wstring path = settings_path();
    if (path.empty()) return false;
    auto puti = [&](const wchar_t* section, const wchar_t* key, int v) {
        wchar_t b[32];
        _snwprintf(b, 32, L"%d", v);
        return WritePrivateProfileStringW(section, key, b, path.c_str()) != FALSE;
    };
    bool ok = true;
    ok &= puti(L"CustomVoice", L"Gender", s.custom.gender);
    ok &= puti(L"CustomVoice", L"Pitch", s.custom.pitch);
    ok &= puti(L"CustomVoice", L"Speed", s.custom.speed);
    ok &= puti(L"CustomVoice", L"Volume", s.custom.volume);
    ok &= puti(L"CustomVoice", L"Brightness", s.custom.bright);
    ok &= puti(L"CustomVoice", L"DollarMode", s.custom.dollar);
    ok &= puti(L"CustomVoice", L"ZeroMode", s.custom.zero);
    ok &= puti(L"CustomVoice", L"NumberSamples", s.custom.number_samples);
    ok &= puti(L"CustomVoice", L"SpreadsheetMode", s.custom.spreadsheet);
    ok &= puti(L"CustomVoice", L"ListMode", s.custom.list);
    ok &= puti(L"General", L"RateBoost", s.rate_boost);
    ok &= puti(L"General", L"Logging", s.logging);
    ok &= puti(L"General", L"WordEvents", s.word_events);
    return ok;
}

}  // namespace ctsettings
