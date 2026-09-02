// Dumps what a screen reader sees in the CyberTalk configuration dialog:
// every child window in tab order with its class, MSAA role, accessible
// name and value, plus a check that each focusable control has a name.
//
//   msaa_dump ["window title"]
#include <windows.h>
#include <oleacc.h>
#include <gdiplus.h>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

static bool png_encoder(CLSID* clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (!size) return false;
    std::vector<BYTE> buf(size);
    auto* info = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; i++)
        if (wcscmp(info[i].MimeType, L"image/png") == 0) { *clsid = info[i].Clsid; return true; }
    return false;
}

// --shot <file.png>: capture the window from the screen
static bool screenshot(HWND dlg, const wchar_t* path) {
    SetForegroundWindow(dlg);
    Sleep(600);
    RECT r;
    GetWindowRect(dlg, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
    SelectObject(mem, old);
    Gdiplus::GdiplusStartupInput si;
    ULONG_PTR tok = 0;
    Gdiplus::GdiplusStartup(&tok, &si, nullptr);
    bool ok = false;
    {
        Gdiplus::Bitmap b(bmp, nullptr);
        CLSID png;
        if (png_encoder(&png)) ok = b.Save(path, &png, nullptr) == Gdiplus::Ok;
    }
    Gdiplus::GdiplusShutdown(tok);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    printf("%s %dx%d -> %S\n", ok ? "saved" : "FAILED to save", w, h, path);
    return ok;
}

static std::wstring role_name(DWORD role) {
    wchar_t buf[128];
    UINT n = GetRoleTextW(role, buf, 128);
    return n ? std::wstring(buf, n) : L"?";
}

static std::wstring bstr_or(BSTR b, const wchar_t* dflt) {
    return b ? std::wstring(b, SysStringLen(b)) : dflt;
}

struct Ctl { HWND hwnd; };

static BOOL CALLBACK enum_children(HWND h, LPARAM lp) {
    reinterpret_cast<std::vector<Ctl>*>(lp)->push_back({ h });
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* title = L"CyberTalk SAPI5 Configuration";
    int press_id = 0, set_id = 0;
    const wchar_t* set_text = nullptr;
    const wchar_t* shot = nullptr;
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--press") == 0 && i + 1 < argc) press_id = _wtoi(argv[++i]);
        else if (wcscmp(argv[i], L"--set") == 0 && i + 2 < argc) { set_id = _wtoi(argv[++i]); set_text = argv[++i]; }
        else if (wcscmp(argv[i], L"--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else title = argv[i];
    }
    SetProcessDPIAware();
    CoInitialize(nullptr);
    HWND dlg = FindWindowW(nullptr, title);
    if (!dlg) {
        printf("window \"%S\" not found\n", title);
        return 1;
    }
    if (shot) return screenshot(dlg, shot) ? 0 : 1;
    if (set_id) {
        HWND c = GetDlgItem(dlg, set_id);
        if (!c) { printf("control %d not found\n", set_id); return 1; }
        SendMessageW(c, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(set_text));
        printf("set control %d to \"%S\"\n", set_id, set_text);
        return 0;
    }
    if (press_id) {
        HWND c = GetDlgItem(dlg, press_id);
        if (!c) { printf("control %d not found\n", press_id); return 1; }
        wchar_t t[64] = L"";
        GetWindowTextW(c, t, 64);
        SendMessageW(c, BM_CLICK, 0, 0);
        printf("pressed control %d (\"%S\")\n", press_id, t);
        return 0;
    }
    std::vector<Ctl> ctls;
    EnumChildWindows(dlg, enum_children, reinterpret_cast<LPARAM>(&ctls));
    printf("%zu child windows\n\n", ctls.size());
    int tab = 0, unnamed = 0;
    for (const Ctl& c : ctls) {
        wchar_t cls[64] = L"", text[128] = L"";
        GetClassNameW(c.hwnd, cls, 64);
        GetWindowTextW(c.hwnd, text, 128);
        LONG style = GetWindowLongW(c.hwnd, GWL_STYLE);
        bool tabstop = (style & WS_TABSTOP) != 0;
        bool visible = (style & WS_VISIBLE) != 0;
        int id = GetDlgCtrlID(c.hwnd);
        IAccessible* acc = nullptr;
        std::wstring name = L"(no IAccessible)", role = L"", value = L"";
        if (SUCCEEDED(AccessibleObjectFromWindow(c.hwnd, OBJID_CLIENT, IID_IAccessible, reinterpret_cast<void**>(&acc))) && acc) {
            VARIANT self;
            self.vt = VT_I4;
            self.lVal = CHILDID_SELF;
            BSTR b = nullptr;
            acc->get_accName(self, &b);
            name = bstr_or(b, L"(no name)");
            if (b) SysFreeString(b);
            VARIANT vr;
            VariantInit(&vr);
            if (SUCCEEDED(acc->get_accRole(self, &vr)) && vr.vt == VT_I4) role = role_name(vr.lVal);
            VariantClear(&vr);
            b = nullptr;
            if (SUCCEEDED(acc->get_accValue(self, &b)) && b) { value = std::wstring(b, SysStringLen(b)); SysFreeString(b); }
            acc->Release();
        }
        if (!visible) continue;
        if (tabstop) tab++;
        bool focusable = tabstop && wcscmp(cls, L"Static") != 0 && wcscmp(cls, L"Button") != 0 ? true : tabstop;
        bool problem = focusable && (name == L"(no name)" || name.empty());
        if (problem) unnamed++;
        printf("%s%2d id=%-5d %-22S role=%-14S name=\"%S\"%s%S%s%s\n",
               tabstop ? "TAB " : "    ", tabstop ? tab : 0, id, cls, role.c_str(), name.c_str(),
               value.empty() ? "" : " value=\"", value.c_str(), value.empty() ? "" : "\"",
               problem ? "   <-- NO ACCESSIBLE NAME" : "");
    }
    printf("\n%d controls in the tab order, %d without an accessible name\n", tab, unnamed);
    CoUninitialize();
    return unnamed ? 2 : 0;
}
