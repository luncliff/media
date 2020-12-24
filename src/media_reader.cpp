#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.Threading.h>

#include <media.hpp>
#include <spdlog/spdlog.h>

#include <shlwapi.h>

using namespace std;

struct critical_section_t final : public CRITICAL_SECTION {
  public:
    critical_section_t() noexcept : CRITICAL_SECTION{} {
        InitializeCriticalSection(this);
    }
    ~critical_section_t() noexcept {
        DeleteCriticalSection(this);
    }
    bool try_lock() noexcept {
        return TryEnterCriticalSection(this);
    }
    void lock() noexcept {
        return EnterCriticalSection(this);
    }
    void unlock() noexcept {
        return LeaveCriticalSection(this);
    }
};

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/processing-media-data-with-the-source-reader
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/using-the-source-reader-in-asynchronous-mode
class callback_impl_t : public IMFSourceReaderCallback {
    /// @see OnReadSample
    struct read_item_t final {
        HRESULT status;
        DWORD index;
        DWORD flags;
        com_ptr<IMFSample> sample;
    };
    static_assert(sizeof(read_item_t) <= 32);

  private:
    critical_section_t mtx{};
    qpc_timer_t timer{};
    com_ptr<IMFPresentationClock> clock{};
    com_ptr<IMFPresentationTimeSource> time_source{};
    SHORT ref_count = 0;

  public:
    callback_impl_t() noexcept(false)
        : IMFSourceReaderCallback{}, mtx{}, timer{}, clock{}, time_source{}, ref_count{0} {
        winrt::check_hresult(MFCreatePresentationClock(clock.put()));
        winrt::check_hresult(MFCreateSystemTimeSource(time_source.put()));
        winrt::check_hresult(clock->SetTimeSource(time_source.get()));
    }

  private:
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent* event) noexcept override {
        UNREFERENCED_PARAMETER(event);
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD) noexcept override {
        return S_OK;
    }
    /// @see https://docs.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsourcereadercallback-onreadsample
    /// @see https://docs.microsoft.com/en-us/windows/win32/api/mfreadwrite/ne-mfreadwrite-mf_source_reader_flag
    STDMETHODIMP OnReadSample(HRESULT status, DWORD index, DWORD flags, LONGLONG timestamp,
                              IMFSample* sample) noexcept override {
        static_assert(sizeof(time_t) == sizeof(LONGLONG));
        if (flags & MF_SOURCE_READERF_ERROR)
            return S_OK;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (auto hr = clock->Stop(); FAILED(hr))
                return hr;
            return S_OK;
        }
        if (flags & MF_SOURCE_READERF_NEWSTREAM)
            return S_OK;
        if (flags & MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)
            return S_OK;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
            return S_OK;
        if (flags & MF_SOURCE_READERF_ALLEFFECTSREMOVED)
            return S_OK;

        if (flags & MF_SOURCE_READERF_STREAMTICK) {
            // sample can be nullptr
            spdlog::debug("OnReadSample: MF_SOURCE_READERF_STREAMTICK");
        } else {
            spdlog::debug("OnReadSample: {}", static_cast<void*>(sample));
            winrt::check_hresult(sample->SetSampleTime(timestamp));
        }
        read_item_t item{};
        item.status = status;
        item.index = index;
        item.sample.attach(sample);
        item.flags = flags;
        return S_OK;
    }

  public:
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) {
        if (*ppv != nullptr)     // if the destination is not `nullptr`,
            return E_INVALIDARG; // it's probably logic error...
        //if (iid == IID_IUnknown) {
        //    *ppv = static_cast<IUnknown*>(this);
        //    return S_OK;
        //}
        //if (iid == IID_IMFSourceReaderCallback) {
        //    *ppv = static_cast<IMFSourceReaderCallback*>(this);
        //    return S_OK;
        //}
        static QITAB table[] = {
            QITABENT(callback_impl_t, IMFSourceReaderCallback),
            {},
        };
        return QISearch(this, table, iid, ppv);
    }
    STDMETHODIMP_(ULONG) AddRef() {
        return InterlockedIncrement16(&ref_count);
    }
    STDMETHODIMP_(ULONG) Release() {
        auto const count = InterlockedDecrement16(&ref_count);
        if (count == 0)
            delete this;
        return count;
    }
};

HRESULT create_reader_callback(IMFSourceReaderCallback** callback) noexcept {
    if (callback == nullptr)
        return E_INVALIDARG;
    try {
        IUnknown* unknown = *callback = new (std::nothrow) callback_impl_t{};
        if (unknown)
            unknown->AddRef();
        return S_OK;
    } catch (const winrt::hresult_error& ex) {
        spdlog::error("create_reader_callback: {}", winrt::to_string(ex.message()));
        return ex.code();
    }
}
