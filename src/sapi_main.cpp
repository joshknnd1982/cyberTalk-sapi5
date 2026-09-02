#include <new>
#include <sapi.h>
#include "com.hpp"
#include "registry.hpp"
#include "ct_log.h"
#include "ISpTTSEngineImpl.hpp"
#include "IEnumSpObjectTokensImpl.hpp"

namespace {

HINSTANCE g_dll_handle = nullptr;
CyberTalk::com::class_object_factory g_cls_obj_factory;

const std::wstring token_enums_path = L"Software\\Microsoft\\Speech\\Voices\\TokenEnums";

void init_logging() {
#ifdef BUILD_X64
    ctlog::Logger::instance().init(L"sapi_x64");
#else
    ctlog::Logger::instance().init(L"sapi_x86");
#endif
}

[[nodiscard]] std::wstring clsid_to_string(const GUID& clsid) {
    wchar_t buf[64];
    StringFromGUID2(clsid, buf, 64);
    return std::wstring(buf);
}

void register_token_enumerator() {
    using namespace CyberTalk::sapi;
    using namespace CyberTalk::registry;
    const std::wstring clsid_str = clsid_to_string(__uuidof(IEnumSpObjectTokensImpl));
    key enums_key(HKEY_LOCAL_MACHINE, token_enums_path, KEY_CREATE_SUB_KEY | KEY_SET_VALUE, true);
    key enum_key(enums_key, L"CyberTalk", KEY_SET_VALUE, true);
    enum_key.set(L"CyberTalk Voices");
    enum_key.set(L"CLSID", clsid_str);
}

void unregister_token_enumerator() noexcept {
    using namespace CyberTalk::registry;
    try {
        key enums_key(HKEY_LOCAL_MACHINE, token_enums_path, KEY_ALL_ACCESS);
        enums_key.delete_subkey(L"CyberTalk");
    } catch (...) {
    }
}

}  // namespace

BOOL APIENTRY DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        g_dll_handle = hInstance;
        DisableThreadLibraryCalls(hInstance);
        try {
            g_cls_obj_factory.register_class<CyberTalk::sapi::IEnumSpObjectTokensImpl>();
            g_cls_obj_factory.register_class<CyberTalk::sapi::ISpTTSEngineImpl>();
        } catch (...) {
            return FALSE;
        }
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    init_logging();
    return g_cls_obj_factory.create(rclsid, riid, ppv);
}

STDAPI DllCanUnloadNow() {
    return CyberTalk::com::object_counter::is_zero() ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    init_logging();
    try {
        CyberTalk::com::class_registrar r(g_dll_handle);
        r.register_class<CyberTalk::sapi::IEnumSpObjectTokensImpl>();
        r.register_class<CyberTalk::sapi::ISpTTSEngineImpl>();
        register_token_enumerator();
        CT_LOG("DllRegisterServer: registered");
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (const std::exception& e) {
        CT_LOG("DllRegisterServer: failed: %s (last error %lu)", e.what(), GetLastError());
        return E_UNEXPECTED;
    } catch (...) {
        CT_LOG("DllRegisterServer: failed");
        return E_UNEXPECTED;
    }
}

STDAPI DllUnregisterServer() {
    init_logging();
    try {
        CyberTalk::sapi::shutdown_host();
        unregister_token_enumerator();
        CyberTalk::com::class_registrar r(g_dll_handle);
        r.unregister_class<CyberTalk::sapi::IEnumSpObjectTokensImpl>();
        r.unregister_class<CyberTalk::sapi::ISpTTSEngineImpl>();
        CT_LOG("DllUnregisterServer: unregistered");
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_UNEXPECTED;
    }
}
