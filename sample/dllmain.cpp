#include <media.hpp>
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

extern "C" {

GSL_SUPPRESS(es .78)
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_instance = instance;
    case DLL_PROCESS_DETACH:
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

STDAPI DllRegisterServer() {
    spdlog::info("register:");
    spdlog::info(" - IMFTransform: {}", to_string(__uuidof(ICustomMFT)));
    return E_NOTIMPL; // regsvr32 will report failure: 0x80004001L
}
STDAPI DllUnregisterServer() {
    spdlog::warn("unregister:");
    return E_NOTIMPL;
}

} // extern "C"
