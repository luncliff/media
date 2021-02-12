#include <media.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include "custom_mft.h" // see custom_mft.idl
#include <inspectable.h>

HINSTANCE g_instance = NULL;
ULONG g_ref_count = 0;

extern "C" {

__declspec(dllexport) ULONG get_current_count() noexcept {
    return g_ref_count;
}

__control_entrypoint(DllExport) STDAPI DllCanUnloadNow() {
    if (g_ref_count == 0) {
        spdlog::warn("DLL can be unloaded");
        return S_OK;
    }
    return S_FALSE;
}

ULONG WINAPI DllAddRef() {
    return InterlockedIncrement(&g_ref_count);
}
ULONG WINAPI DllRelease() {
    return InterlockedDecrement(&g_ref_count);
}
} // extern "C"

class class_factory_t : public IClassFactory {
    LONG ref_count = 1;

  public:
    class_factory_t() noexcept {
        spdlog::debug("ctor: class_factory_t");
        DllAddRef();
    }

    ~class_factory_t() noexcept {
        spdlog::warn("dtor: class_factory_t");
        DllRelease();
    }

  public:
    // IClassFactory Methods
    IFACEMETHODIMP CreateInstance(_In_ IUnknown* unknown, _In_ REFIID iid, _Outptr_ void** ppv) {
        if (ppv == nullptr)
            return E_POINTER;
        UNREFERENCED_PARAMETER(unknown);
        UNREFERENCED_PARAMETER(iid);
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    IFACEMETHODIMP LockServer(BOOL lock) {
        if (lock)
            DllAddRef();
        else
            DllRelease();
        return S_OK;
    }

    IFACEMETHODIMP QueryInterface(REFIID iid, void** ppv) {
        if (ppv == nullptr)
            return E_POINTER;

        if (iid == IID_IUnknown)
            *ppv = static_cast<IUnknown*>(this);
        else if (iid == IID_IClassFactory)
            *ppv = static_cast<IClassFactory*>(this);
        else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        this->AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() {
        return InterlockedIncrement(&ref_count);
    }

    IFACEMETHODIMP_(ULONG) Release() {
        const auto count = InterlockedDecrement(&ref_count);
        if (count == 0)
            delete this;
        return count;
    }
};

void registry_set_value(HKEY hkey, DWORD value) {
    if (auto ec = RegSetValueExW(hkey, NULL, 0, REG_DWORD, //
                                 reinterpret_cast<const BYTE*>(&value), sizeof(value)))
        spdlog::error("set(REG_DWORD): {}", ec);
}
void registry_set_value(HKEY hkey, std::wstring text) {
    if (auto ec = RegSetValueExW(hkey, NULL, 0, REG_SZ, //
                                 reinterpret_cast<const BYTE*>(text.c_str()),
                                 sizeof(std::wstring::value_type) * text.length()))
        spdlog::error("set(REG_SZ): {}", ec);
}

template <typename T>
bool registry_create_key(HKEY hkey, std::wstring subkey, T&& value) noexcept {
    switch (auto status = RegCreateKeyExW(hkey, subkey.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL,
                                          &hkey, NULL)) {
    case ERROR_SUCCESS:
        break;
    case ERROR_ACCESS_DENIED:     // 5
    case ERROR_INVALID_PARAMETER: // 87
    default:
        return false;
    }
    registry_set_value(hkey, value);
    return RegCloseKey(hkey) == ERROR_SUCCESS;
}
template <typename T>
bool registry_create_key(std::wstring subkey, T&& value) noexcept {
    return registry_create_key(HKEY_CURRENT_USER, std::move(subkey), std::forward<T&&>(value));
}

bool registry_delete_key(HKEY hkey, std::wstring subkey) noexcept {
    switch (auto ec = RegDeleteKeyW(hkey, subkey.c_str())) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_SUCCESS:
        return true;
    default:
        return false;
    }
}
bool registry_delete_key(std::wstring subkey) noexcept {
    return registry_delete_key(HKEY_CURRENT_USER, std::move(subkey));
}

std::wstring get_module_path(HMODULE mod) noexcept {
    WCHAR buf[MAX_PATH]{};
    DWORD buflen = GetModuleFileNameW(mod, buf, MAX_PATH);
    return {buf, buflen};
}
std::wstring to_wstring(const GUID& guid) noexcept {
    constexpr auto bufsz = 40;
    wchar_t buf[bufsz]{};
    uint32_t buflen = StringFromGUID2(guid, buf, bufsz);
    return {buf, buflen};
}

BOOL setup() noexcept {
    try {
        auto log = spdlog::basic_logger_st("file", "a.log");
        spdlog::set_default_logger(log);
        spdlog::set_pattern("[%^%l%$] %v");
        spdlog::set_level(spdlog::level::level_enum::debug);
        return TRUE;
    } catch (const std::exception& ex) {
        fputs(ex.what(), stderr);
        return FALSE;
    }
}

extern "C" {

GSL_SUPPRESS(es .78)
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        // winrt::init_apartment();
        g_instance = instance;
        return setup();
    case DLL_PROCESS_DETACH:
        // winrt::uninit_apartment();
    default:
        break;
    }
    return TRUE;
}

_Check_return_ STDAPI DllGetClassObject(_In_ REFCLSID clsid, _In_ REFIID iid, _Outptr_ LPVOID FAR* ppv) {
    const GUID& CLSID_ICustomMFT = get_CLSID_MFT();
    if (clsid != CLSID_ICustomMFT)
        return CLASS_E_CLASSNOTAVAILABLE;

    auto factory = new (std::nothrow) class_factory_t{};
    if (factory == nullptr)
        return E_OUTOFMEMORY;
    auto on_return = gsl::finally([factory]() { factory->Release(); });
    return factory->QueryInterface(iid, ppv);
}

#define REQUIRE(expr)                                                                                                  \
    if (expr == false)                                                                                                 \
        return E_FAIL;

/// @note `regsvr32` will report from this function's return
STDAPI DllRegisterServer() {
    const auto program_id = std::wstring{L"keep.test.0"};
    const auto clsid_mft0 = to_wstring(get_CLSID_MFT());
    const auto libpath = get_module_path(g_instance);
    const auto comment = L"custom_mft with DLL";
    REQUIRE(registry_create_key(program_id, comment));
    REQUIRE(registry_create_key(program_id + L"\\CLSID", clsid_mft0));
    REQUIRE(registry_create_key(HKEY_CLASSES_ROOT, clsid_mft0, comment));
    REQUIRE(registry_create_key(HKEY_CLASSES_ROOT, clsid_mft0 + L"\\ProgID", program_id));
    REQUIRE(registry_create_key(HKEY_CLASSES_ROOT, clsid_mft0 + L"\\InProcServer32", libpath));
    return S_OK;
}

/// @note `regsvr32 /u` will report from this function's return
STDAPI DllUnregisterServer() {
    const auto program_id = std::wstring{L"keep.test.0"};
    const auto clsid_mft0 = to_wstring(get_CLSID_MFT());
    REQUIRE(registry_delete_key(HKEY_CLASSES_ROOT, clsid_mft0 + L"\\InProcServer32"));
    REQUIRE(registry_delete_key(HKEY_CLASSES_ROOT, clsid_mft0 + L"\\ProgID"));
    REQUIRE(registry_delete_key(HKEY_CLASSES_ROOT, clsid_mft0));
    REQUIRE(registry_delete_key(program_id + L"\\CLSID"));
    REQUIRE(registry_delete_key(program_id));
    return S_OK;
}
#undef REQUIRE

} // extern "C"
