#include <media.hpp>
#include <spdlog/spdlog.h>

#include <custom_mft.h> // see custom_mft.idl
#include <inspectable.h>

HINSTANCE g_instance = NULL;
ULONG g_ref_count = 0;

extern "C" {

// CLSID of the MFT.
// {1C2CE17A-FAAD-4E73-85E7-167068093F25}
const GUID CLSID_ICustomMFT{0x1c2ce17a, 0xfaad, 0x4e73, 0x85, 0xe7, 0x16, 0x70, 0x68, 0x9, 0x3f, 0x25};
const GUID IID_ICustomMFT = __uuidof(ICustomMFT);

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
        DllAddRef();
        spdlog::info("supports:");
        spdlog::info(" - IMFTransform: {}", to_string(IID_ICustomMFT));
    }

    ~class_factory_t() noexcept {
        DllRelease();
    }

  public:
    // IClassFactory Methods
    IFACEMETHODIMP CreateInstance(_In_ IUnknown* unknown, _In_ REFIID iid, _Outptr_ void** ppv) {
        if (ppv = nullptr)
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
    if (clsid != CLSID_ICustomMFT)
        return CLASS_E_CLASSNOTAVAILABLE;

    auto factory = new (std::nothrow) class_factory_t{};
    if (factory == nullptr)
        return E_OUTOFMEMORY;
    auto on_return = gsl::finally([factory]() { factory->Release(); });
    return factory->QueryInterface(iid, ppv);
}

} // extern "C"
