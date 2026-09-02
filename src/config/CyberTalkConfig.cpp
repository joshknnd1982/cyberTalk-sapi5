// CyberTalk SAPI5 configuration utility.
//
// Adjusts every parameter of the "CyberTalk Custom Voice" (gender, pitch,
// speed, volume, brightness, reading modes) plus general wrapper options, and
// previews the result through the CyberTalk host.  Screen-reader friendly:
// every control is a standard Win32 control with a text label preceding it in
// the tab order, all controls are reachable with Tab, and each slider is
// paired with a spin edit showing the same 0..100 percentage (0 = engine
// minimum, 100 = engine maximum).
#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "resource.h"
#include "ct_log.h"
#include "ct_settings.h"
#include "pipe_client.h"
#include "stl_text.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")

namespace {

HINSTANCE g_hinst = nullptr;
HWND g_dlg = nullptr;
bool g_loading = true;   // guards control-change handlers until the dialog is fully initialised
ctsettings::Settings g_settings;

struct Slider {
    int slider, edit, spin, info;
};
const Slider SLIDERS[] = {
    // "info" is the label that sits right before the spin edit; it names the
    // edit for screen readers and carries the real engine value
    { IDC_PITCH_SLIDER, IDC_PITCH_EDIT, IDC_PITCH_SPIN, IDC_LBL_PITCH_PCT },
    { IDC_SPEED_SLIDER, IDC_SPEED_EDIT, IDC_SPEED_SPIN, IDC_LBL_SPEED_PCT },
    { IDC_VOLUME_SLIDER, IDC_VOLUME_EDIT, IDC_VOLUME_SPIN, IDC_LBL_VOLUME_PCT },
    { IDC_BRIGHT_SLIDER, IDC_BRIGHT_EDIT, IDC_BRIGHT_SPIN, IDC_LBL_BRIGHT_PCT },
};

int current_gender() {
    return SendDlgItemMessageW(g_dlg, IDC_GENDER, CB_GETCURSEL, 0, 0) == 1 ? 1 : 2;
}

int pct_to_value(int id, int pct) {
    pct = ctsettings::clampi(pct, 0, 100);
    switch (id) {
    case IDC_PITCH_SLIDER: {
        int g = current_gender();
        int lo = ctsettings::pitch_min(g), hi = ctsettings::pitch_max(g);
        return lo + (hi - lo) * pct / 100;
    }
    case IDC_SPEED_SLIDER: return 100 + 200 * pct / 100;
    case IDC_VOLUME_SLIDER: return 255 * pct / 100;
    case IDC_BRIGHT_SLIDER: return (15 * pct + 50) / 100;
    }
    return pct;
}

int value_to_pct(int id, int value) {
    switch (id) {
    case IDC_PITCH_SLIDER: {
        int g = current_gender();
        int lo = ctsettings::pitch_min(g), hi = ctsettings::pitch_max(g);
        value = ctsettings::clampi(value, lo, hi);
        return ((value - lo) * 100 + (hi - lo) / 2) / (hi - lo);
    }
    case IDC_SPEED_SLIDER: return ((ctsettings::clampi(value, 100, 300) - 100) * 100 + 100) / 200;
    case IDC_VOLUME_SLIDER: return (ctsettings::clampi(value, 0, 255) * 100 + 127) / 255;
    case IDC_BRIGHT_SLIDER: return (ctsettings::clampi(value, 0, 15) * 100 + 7) / 15;
    }
    return value;
}

void update_info(const Slider& s) {
    int pct = static_cast<int>(SendDlgItemMessageW(g_dlg, s.slider, TBM_GETPOS, 0, 0));
    int v = pct_to_value(s.slider, pct);
    wchar_t buf[64];
    switch (s.slider) {
    case IDC_PITCH_SLIDER: _snwprintf(buf, 64, L"Pitch %% (%d Hz):", v); break;
    case IDC_SPEED_SLIDER: _snwprintf(buf, 64, L"Speed %% (%d words/min):", v); break;
    case IDC_VOLUME_SLIDER: _snwprintf(buf, 64, L"Volume %% (%d of 255):", v); break;
    case IDC_BRIGHT_SLIDER: _snwprintf(buf, 64, L"Brightness %% (level %d of 15):", v); break;
    default: buf[0] = 0;
    }
    SetDlgItemTextW(g_dlg, s.info, buf);
}

void set_pct(const Slider& s, int pct) {
    pct = ctsettings::clampi(pct, 0, 100);
    bool was = g_loading;
    g_loading = true;
    SendDlgItemMessageW(g_dlg, s.slider, TBM_SETPOS, TRUE, pct);
    SendDlgItemMessageW(g_dlg, s.spin, UDM_SETPOS32, 0, pct);
    g_loading = was;
    update_info(s);
}

int get_pct(const Slider& s) {
    return static_cast<int>(SendDlgItemMessageW(g_dlg, s.slider, TBM_GETPOS, 0, 0));
}

const Slider* slider_by_id(int id) {
    for (const Slider& s : SLIDERS)
        if (s.slider == id || s.edit == id || s.spin == id) return &s;
    return nullptr;
}

void set_status(const wchar_t* text) {
    SetDlgItemTextW(g_dlg, IDC_STATUS, text);
    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, GetDlgItem(g_dlg, IDC_STATUS), OBJID_CLIENT, CHILDID_SELF);
}

// ------------------------------------------------------------- settings <-> UI
void load_ui_from(const ctsettings::Settings& s) {
    g_loading = true;
    SendDlgItemMessageW(g_dlg, IDC_GENDER, CB_SETCURSEL, s.custom.gender == 1 ? 1 : 0, 0);
    g_loading = false;
    set_pct(SLIDERS[0], value_to_pct(IDC_PITCH_SLIDER, s.custom.pitch));
    set_pct(SLIDERS[1], value_to_pct(IDC_SPEED_SLIDER, s.custom.speed));
    set_pct(SLIDERS[2], value_to_pct(IDC_VOLUME_SLIDER, s.custom.volume));
    set_pct(SLIDERS[3], value_to_pct(IDC_BRIGHT_SLIDER, s.custom.bright));
    CheckDlgButton(g_dlg, IDC_DOLLAR, s.custom.dollar ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_dlg, IDC_ZERO, s.custom.zero ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_dlg, IDC_NUMBERS, s.custom.number_samples ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_dlg, IDC_SPREADSHEET, s.custom.spreadsheet ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_dlg, IDC_LIST, s.custom.list ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_dlg, IDC_RATEBOOST, s.rate_boost ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_dlg, IDC_WORDEVENTS, s.word_events ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_dlg, IDC_LOGGING, s.logging ? BST_CHECKED : BST_UNCHECKED);
}

ctsettings::Settings read_ui() {
    ctsettings::Settings s = g_settings;
    s.custom.gender = current_gender();
    s.custom.pitch = pct_to_value(IDC_PITCH_SLIDER, get_pct(SLIDERS[0]));
    s.custom.speed = pct_to_value(IDC_SPEED_SLIDER, get_pct(SLIDERS[1]));
    s.custom.volume = pct_to_value(IDC_VOLUME_SLIDER, get_pct(SLIDERS[2]));
    s.custom.bright = pct_to_value(IDC_BRIGHT_SLIDER, get_pct(SLIDERS[3]));
    s.custom.dollar = IsDlgButtonChecked(g_dlg, IDC_DOLLAR) == BST_CHECKED;
    s.custom.zero = IsDlgButtonChecked(g_dlg, IDC_ZERO) == BST_CHECKED;
    s.custom.number_samples = IsDlgButtonChecked(g_dlg, IDC_NUMBERS) == BST_CHECKED;
    s.custom.spreadsheet = IsDlgButtonChecked(g_dlg, IDC_SPREADSHEET) == BST_CHECKED;
    s.custom.list = IsDlgButtonChecked(g_dlg, IDC_LIST) == BST_CHECKED;
    s.rate_boost = IsDlgButtonChecked(g_dlg, IDC_RATEBOOST) == BST_CHECKED;
    s.word_events = IsDlgButtonChecked(g_dlg, IDC_WORDEVENTS) == BST_CHECKED;
    s.logging = IsDlgButtonChecked(g_dlg, IDC_LOGGING) == BST_CHECKED;
    ctsettings::sanitize(s);
    return s;
}

// ------------------------------------------------------------------ preview
struct Player {
    HWAVEOUT hwo = nullptr;
    std::mutex mtx;
    std::vector<WAVEHDR*> headers;
    std::atomic<int> queued{ 0 };

    bool open() {
        WAVEFORMATEX f = {};
        f.wFormatTag = WAVE_FORMAT_PCM;
        f.nChannels = 1;
        f.nSamplesPerSec = 11025;
        f.wBitsPerSample = 16;
        f.nBlockAlign = 2;
        f.nAvgBytesPerSec = 22050;
        return waveOutOpen(&hwo, WAVE_MAPPER, &f, reinterpret_cast<DWORD_PTR>(callback), reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION) == MMSYSERR_NOERROR;
    }
    static void CALLBACK callback(HWAVEOUT, UINT msg, DWORD_PTR inst, DWORD_PTR param1, DWORD_PTR) {
        if (msg != WOM_DONE) return;
        Player* p = reinterpret_cast<Player*>(inst);
        WAVEHDR* h = reinterpret_cast<WAVEHDR*>(param1);
        (void)h;
        p->queued--;
    }
    void write(const void* data, uint32_t size) {
        if (!hwo || size == 0) return;
        // keep no more than ~2 s queued so Stop responds quickly
        while (queued.load() > 4) Sleep(20);
        WAVEHDR* h = new WAVEHDR();
        memset(h, 0, sizeof(WAVEHDR));
        h->lpData = static_cast<LPSTR>(malloc(size));
        memcpy(h->lpData, data, size);
        h->dwBufferLength = size;
        if (waveOutPrepareHeader(hwo, h, sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
            queued++;
            if (waveOutWrite(hwo, h, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) queued--;
        }
        std::lock_guard<std::mutex> lock(mtx);
        headers.push_back(h);
    }
    void stop() {
        if (hwo) waveOutReset(hwo);
    }
    void drain() {
        DWORD t0 = GetTickCount();
        while (queued.load() > 0 && GetTickCount() - t0 < 60000) Sleep(20);
    }
    void close() {
        if (!hwo) return;
        waveOutReset(hwo);
        std::lock_guard<std::mutex> lock(mtx);
        for (WAVEHDR* h : headers) {
            waveOutUnprepareHeader(hwo, h, sizeof(WAVEHDR));
            free(h->lpData);
            delete h;
        }
        headers.clear();
        waveOutClose(hwo);
        hwo = nullptr;
    }
};

std::atomic<bool> g_preview_running{ false };
std::atomic<bool> g_preview_stop{ false };
Player* g_player = nullptr;
std::mutex g_player_mutex;

struct PreviewJob {
    SpeakCommand cmd;
    std::string text;
};

DWORD WINAPI preview_thread(LPVOID p) {
    PreviewJob* job = static_cast<PreviewJob*>(p);
    Player player;
    if (!player.open()) {
        PostMessageW(g_dlg, WM_APP + 1, 0, reinterpret_cast<LPARAM>(new std::wstring(L"Could not open the audio device.")));
        delete job;
        g_preview_running = false;
        return 0;
    }
    {
        std::lock_guard<std::mutex> lock(g_player_mutex);
        g_player = &player;
    }
    PipeClient client;
    std::wstring status;
    if (!client.connect()) {
        status = L"Could not start the CyberTalk host: " + std::wstring(client.last_error().begin(), client.last_error().end());
    } else {
        PipeClient::SpeakEvents ev;
        ev.audio = [&](const void* data, uint32_t size) -> bool {
            if (g_preview_stop) return false;
            player.write(data, size);
            return !g_preview_stop;
        };
        SpeakEndResponse end;
        DWORD t0 = GetTickCount();
        bool ok = client.speak(job->cmd, job->text, ev, end);
        if (!g_preview_stop) player.drain();
        wchar_t b[160];
        if (!ok) _snwprintf(b, 160, L"Preview failed: %S", client.last_error().c_str());
        else if (end.status == 1 || g_preview_stop) _snwprintf(b, 160, L"Preview stopped.");
        else if (end.status == 2) _snwprintf(b, 160, L"Engine error 0x%08lX", (unsigned long)end.hresult);
        else _snwprintf(b, 160, L"Preview finished (%.1f s of audio in %lu ms).", end.total_bytes / 22050.0, GetTickCount() - t0);
        status = b;
    }
    {
        std::lock_guard<std::mutex> lock(g_player_mutex);
        g_player = nullptr;
    }
    player.close();
    delete job;
    g_preview_running = false;
    PostMessageW(g_dlg, WM_APP + 1, 0, reinterpret_cast<LPARAM>(new std::wstring(status)));
    return 0;
}

void start_preview() {
    if (g_preview_running) return;
    ctsettings::Settings s = read_ui();
    wchar_t text[2048];
    GetDlgItemTextW(g_dlg, IDC_TESTTEXT, text, 2048);
    if (!text[0]) {
        set_status(L"Enter some test text first.");
        return;
    }
    PreviewJob* job = new PreviewJob();
    job->cmd = SpeakCommand{};
    job->cmd.gender = s.custom.gender;
    job->cmd.pitch = s.custom.pitch;
    job->cmd.speed = s.custom.speed;
    job->cmd.volume = s.custom.volume;
    job->cmd.bright = s.custom.bright;
    job->cmd.modes = (s.custom.dollar ? MODE_FLAG_DOLLAR : 0) | (s.custom.zero ? MODE_FLAG_ZERO : 0) |
                     (s.custom.number_samples ? MODE_FLAG_NUMBER_SAMPLES : 0) | (s.custom.spreadsheet ? MODE_FLAG_SPREADSHEET : 0) |
                     (s.custom.list ? MODE_FLAG_LIST : 0);
    job->cmd.sonic_speed = 1.0f;
    std::vector<unsigned> offsets;
    job->text = stl::wide_to_engine_text(text, wcslen(text), false, nullptr);
    g_preview_stop = false;
    g_preview_running = true;
    set_status(L"Speaking...");
    HANDLE th = CreateThread(nullptr, 0, preview_thread, job, 0, nullptr);
    if (th) CloseHandle(th);
    else { g_preview_running = false; delete job; }
}

void stop_preview() {
    g_preview_stop = true;
    std::lock_guard<std::mutex> lock(g_player_mutex);
    if (g_player) g_player->stop();
}

// ------------------------------------------------------------------- dialog
void init_dialog(HWND hwnd) {
    g_dlg = hwnd;
    HICON icon = LoadIconW(g_hinst, MAKEINTRESOURCEW(IDI_APP));
    if (icon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    }
    SendDlgItemMessageW(hwnd, IDC_GENDER, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Male"));
    SendDlgItemMessageW(hwnd, IDC_GENDER, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Female"));
    for (const Slider& s : SLIDERS) {
        SendDlgItemMessageW(hwnd, s.slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendDlgItemMessageW(hwnd, s.slider, TBM_SETTICFREQ, 10, 0);
        SendDlgItemMessageW(hwnd, s.slider, TBM_SETPAGESIZE, 0, 10);
        SendDlgItemMessageW(hwnd, s.slider, TBM_SETLINESIZE, 0, 1);
        SendDlgItemMessageW(hwnd, s.spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(GetDlgItem(hwnd, s.edit)), 0);
        SendDlgItemMessageW(hwnd, s.spin, UDM_SETRANGE32, 0, 100);
        SendDlgItemMessageW(hwnd, s.edit, EM_LIMITTEXT, 3, 0);
    }
    SetDlgItemTextW(hwnd, IDC_TESTTEXT, L"Hello. This is the Panasonic CyberTalk text to speech engine. The total is $19.99 for 42 items, in room 101.");
    ctsettings::load(g_settings);
    load_ui_from(g_settings);
    g_loading = false;
    set_status(L"");
    SetFocus(GetDlgItem(hwnd, IDC_GENDER));
}

void on_gender_changed() {
    // keep the same percentage so the pitch stays at the same relative position
    int pct = get_pct(SLIDERS[0]);
    set_pct(SLIDERS[0], pct);
    for (const Slider& s : SLIDERS) update_info(s);
}

void reset_defaults() {
    ctsettings::Settings d;
    d.custom.gender = current_gender();
    d.custom.pitch = ctsettings::pitch_default(d.custom.gender);
    d.custom.bright = ctsettings::bright_default(d.custom.gender);
    load_ui_from(d);
    set_status(L"Defaults restored (not saved yet).");
}

void save_settings() {
    ctsettings::Settings s = read_ui();
    if (ctsettings::save(s)) {
        g_settings = s;
        ctlog::Logger::instance().set_enabled(s.logging != 0);
        CT_LOG("settings saved: gender=%d pitch=%d speed=%d volume=%d bright=%d dollar=%d zero=%d numbers=%d spreadsheet=%d list=%d rate_boost=%d word_events=%d logging=%d",
               s.custom.gender, s.custom.pitch, s.custom.speed, s.custom.volume, s.custom.bright, s.custom.dollar, s.custom.zero,
               s.custom.number_samples, s.custom.spreadsheet, s.custom.list, s.rate_boost, s.word_events, s.logging);
        set_status(L"Settings saved.");
        // a message box is announced reliably by screen readers; a changed
        // static label is not
        MessageBoxW(g_dlg, L"Settings saved. Applications using the CyberTalk Custom Voice pick them up with the next utterance.",
                    L"CyberTalk", MB_ICONINFORMATION | MB_OK);
    } else {
        set_status(L"Saving failed.");
        MessageBoxW(g_dlg, L"The settings file could not be written.", L"CyberTalk", MB_ICONERROR | MB_OK);
    }
}

INT_PTR CALLBACK dialog_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG:
        init_dialog(hwnd);
        return FALSE;   // focus set manually
    case WM_HSCROLL: {
        if (g_loading) return TRUE;
        HWND ctl = reinterpret_cast<HWND>(lParam);
        int id = GetDlgCtrlID(ctl);
        const Slider* s = slider_by_id(id);
        if (s && id == s->slider) {
            int pct = static_cast<int>(SendMessageW(ctl, TBM_GETPOS, 0, 0));
            bool was = g_loading;
            g_loading = true;
            SendDlgItemMessageW(hwnd, s->spin, UDM_SETPOS32, 0, pct);
            g_loading = was;
            update_info(*s);
        }
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam), code = HIWORD(wParam);
        if (g_loading && id != IDCANCEL) return TRUE;
        const Slider* s = slider_by_id(id);
        if (s && id == s->edit && code == EN_CHANGE) {
            wchar_t buf[16];
            GetDlgItemTextW(hwnd, s->edit, buf, 16);
            if (buf[0]) {
                int pct = ctsettings::clampi(_wtoi(buf), 0, 100);
                bool was = g_loading;
                g_loading = true;
                SendDlgItemMessageW(hwnd, s->slider, TBM_SETPOS, TRUE, pct);
                g_loading = was;
                update_info(*s);
            }
            return TRUE;
        }
        if (s && id == s->edit && code == EN_KILLFOCUS) {
            set_pct(*s, get_pct(*s));   // normalise the text (clamp, remove leading zeros)
            return TRUE;
        }
        switch (id) {
        case IDC_GENDER:
            if (code == CBN_SELCHANGE) on_gender_changed();
            return TRUE;
        case IDC_PREVIEW: start_preview(); return TRUE;
        case IDC_STOP: stop_preview(); return TRUE;
        case IDC_SAVE: save_settings(); return TRUE;
        case IDC_DEFAULTS: reset_defaults(); return TRUE;
        case IDC_OPENLOGS: {
            std::wstring dir = ctlog::Logger::instance().dir();
            ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        case IDCANCEL:
            stop_preview();
            EndDialog(hwnd, 0);
            return TRUE;
        }
        return FALSE;
    }
    case WM_APP + 1: {
        std::wstring* s = reinterpret_cast<std::wstring*>(lParam);
        set_status(s->c_str());
        delete s;
        return TRUE;
    }
    case WM_CLOSE:
        stop_preview();
        EndDialog(hwnd, 0);
        return TRUE;
    }
    return FALSE;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    g_hinst = hInstance;
    ctlog::Logger::instance().init(L"config");
    {
        ctsettings::Settings s;
        ctsettings::load(s);
        ctlog::Logger::instance().set_enabled(s.logging != 0);
    }
    CT_LOG("=== configuration utility started ===");
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    CoInitialize(nullptr);
    DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_MAIN), nullptr, dialog_proc, 0);
    CoUninitialize();
    CT_LOG("=== configuration utility closed ===");
    return 0;
}
