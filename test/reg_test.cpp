// Runs CyberTalkSAPI.dll's DllRegisterServer / DllUnregisterServer with
// HKEY_LOCAL_MACHINE and HKEY_CLASSES_ROOT redirected into
// HKCU\Software\CTRegTest so the registration code can be verified without
// administrator rights.
//
//   reg_test <dll path> [unregister]
#include <windows.h>
#include <cstdio>
#include <initializer_list>

typedef HRESULT(STDAPICALLTYPE* RegFn)();

static HKEY make_key(const wchar_t* sub) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &k, nullptr) != ERROR_SUCCESS) return nullptr;
    return k;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        printf("usage: reg_test <dll> [unregister]\n");
        return 2;
    }
    HKEY hklm = make_key(L"Software\\CTRegTest\\HKLM");
    HKEY hkcr = make_key(L"Software\\CTRegTest\\HKCR");
    if (!hklm || !hkcr) {
        printf("could not create redirect keys\n");
        return 1;
    }
    // keys that always exist in a real registry
    for (const wchar_t* sub : { L"Software\\CTRegTest\\HKLM\\Software\\Classes\\CLSID",
                                L"Software\\CTRegTest\\HKLM\\Software\\Microsoft\\Speech\\Voices\\TokenEnums",
                                L"Software\\CTRegTest\\HKCR\\CLSID" }) {
        HKEY k = make_key(sub);
        if (k) RegCloseKey(k);
    }
    RegOverridePredefKey(HKEY_LOCAL_MACHINE, hklm);
    RegOverridePredefKey(HKEY_CLASSES_ROOT, hkcr);
    HMODULE m = LoadLibraryW(argv[1]);
    if (!m) {
        printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    const char* name = argc > 2 ? "DllUnregisterServer" : "DllRegisterServer";
    RegFn fn = reinterpret_cast<RegFn>(GetProcAddress(m, name));
    if (!fn) {
        printf("%s not exported\n", name);
        return 1;
    }
    HRESULT hr = fn();
    printf("%s -> 0x%08lX\n", name, hr);
    RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
    RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr);
    RegCloseKey(hklm);
    RegCloseKey(hkcr);
    return SUCCEEDED(hr) ? 0 : 1;
}
