#include <media.hpp>
#include <spdlog/spdlog.h>

#include <shlwapi.h>

using namespace std;

/// @todo use `static_assert` for Windows SDK
class qpc_timer_t final {
    LARGE_INTEGER start{};
    LARGE_INTEGER frequency{};

  public:
    qpc_timer_t() noexcept {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
    }

    /// @return elapsed time in millisecond unit
    auto pick() const noexcept {
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&end);
        const auto elapsed = end.QuadPart - start.QuadPart;
        return (elapsed * 1'000) / frequency.QuadPart;
    }

    auto reset() noexcept {
        auto d = pick();
        QueryPerformanceCounter(&start);
        return d;
    }
};
static_assert(sizeof(time_t) == sizeof(LONGLONG));

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
/// @see https://docs.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsourcereadercallback-onreadsample
/// @see https://docs.microsoft.com/en-us/windows/win32/api/mfreadwrite/ne-mfreadwrite-mf_source_reader_flag
class verbose_callback_t : public IMFSourceReaderCallback {
    critical_section_t mtx{};
    com_ptr<IMFSourceReader> reader{};
    qpc_timer_t timer{};
    com_ptr<IMFPresentationClock> clock{};
    com_ptr<IMFPresentationTimeSource> time_source{};
    SHORT ref_count = 0;

  public:
    verbose_callback_t() noexcept(false)
        : IMFSourceReaderCallback{}, mtx{}, reader{}, timer{}, clock{}, time_source{}, ref_count{0} {
        winrt::check_hresult(MFCreatePresentationClock(clock.put()));
        winrt::check_hresult(MFCreateSystemTimeSource(time_source.put()));
        winrt::check_hresult(clock->SetTimeSource(time_source.get()));
    }

  private:
    STDMETHODIMP OnEvent(DWORD stream, IMFMediaEvent* event) noexcept override {
        UNREFERENCED_PARAMETER(stream);
        UNREFERENCED_PARAMETER(event);
        spdlog::debug("{}", __FUNCTION__);
        lock_guard lck{mtx};
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD stream) noexcept override {
        UNREFERENCED_PARAMETER(stream);
        spdlog::debug("{}", __FUNCTION__);
        lock_guard lck{mtx};
        reader = nullptr;
        return S_OK;
    }
    STDMETHODIMP OnReadSample(HRESULT status, DWORD stream, DWORD flags, //
                              LONGLONG timestamp, IMFSample* sample) noexcept override {
        spdlog::debug("{}: {:#x}", __FUNCTION__, flags);
        lock_guard lck{mtx};
        if (flags & MF_SOURCE_READERF_ERROR)
            return status;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            return S_OK;
        if (flags & MF_SOURCE_READERF_NEWSTREAM)
            return S_OK;
        if (flags & MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)
            return S_OK;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
            return S_OK;
        if (flags & MF_SOURCE_READERF_ALLEFFECTSREMOVED)
            return S_OK;

        if (sample != nullptr) {
            winrt::check_hresult(sample->SetSampleTime(timestamp));
            com_ptr<IMFMediaBuffer> buffer{};
            winrt::check_hresult(sample->GetBufferByIndex(0, buffer.put()));
            DWORD buflen = 0;
            winrt::check_hresult(buffer->GetCurrentLength(&buflen));
            spdlog::debug("  buflen: {}", buflen);
        }

        if (reader == nullptr)
            return status;
        return reader->ReadSample(stream, 0, NULL, NULL, NULL, NULL);
    }

  public:
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) {
        const QITAB table[]{
            QITABENT(verbose_callback_t, IMFSourceReaderCallback),
            {},
        };
        return QISearch(this, table, iid, ppv);
    }
    STDMETHODIMP_(ULONG) AddRef() {
        return InterlockedIncrement16(&ref_count);
    }
    STDMETHODIMP_(ULONG) Release() {
        const auto count = InterlockedDecrement16(&ref_count);
        if (count == 0)
            delete this;
        return count;
    }
};

HRESULT create_reader_callback(IMFSourceReaderCallback** ptr) noexcept {
    auto report_failure = [](HRESULT code, string&& message) {
        spdlog::error("create_reader_callback: {}", message);
        return code;
    };

    if (ptr == nullptr)
        return E_INVALIDARG;
    try {
        if (IUnknown* unknown = *ptr = new (nothrow) verbose_callback_t{})
            unknown->AddRef();
        return S_OK;
    } catch (const winrt::hresult_error& ex) {
        return report_failure(ex.code(), winrt::to_string(ex.message()));
    } catch (...) {
        return report_failure(E_FAIL, "unknown");
    }
}

class save_image_sink_t : public IMFMediaSink {
    LONG ref_count = 0;

  public:
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) {
        const QITAB table[]{
            QITABENT(save_image_sink_t, IMFMediaSink),
            {},
        };
        return QISearch(this, table, iid, ppv);
    }
    STDMETHODIMP_(ULONG) AddRef() {
        return InterlockedDecrement(&ref_count);
    }
    STDMETHODIMP_(ULONG) Release() {
        const auto count = InterlockedDecrement(&ref_count);
        if (count == 0)
            delete this;
        return count;
    }

    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* pdwCharacteristics) override {
        UNREFERENCED_PARAMETER(pdwCharacteristics);
        spdlog::trace("{}", __FUNCTION__);
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE AddStreamSink(DWORD dwStreamSinkIdentifier, IMFMediaType* pMediaType,
                                            IMFStreamSink** ppStreamSink) override {
        UNREFERENCED_PARAMETER(dwStreamSinkIdentifier);
        UNREFERENCED_PARAMETER(pMediaType);
        UNREFERENCED_PARAMETER(ppStreamSink);
        spdlog::trace("{}", __FUNCTION__);
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE RemoveStreamSink(DWORD dwStreamSinkIdentifier) override {
        UNREFERENCED_PARAMETER(dwStreamSinkIdentifier);
        spdlog::trace("{}", __FUNCTION__);
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetStreamSinkCount(DWORD* pcStreamSinkCount) override {
        UNREFERENCED_PARAMETER(pcStreamSinkCount);
        spdlog::trace("{}", __FUNCTION__);
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetStreamSinkByIndex(DWORD dwIndex, IMFStreamSink** ppStreamSink) override {
        UNREFERENCED_PARAMETER(dwIndex);
        UNREFERENCED_PARAMETER(ppStreamSink);
        spdlog::trace("{}", __FUNCTION__);
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetStreamSinkById(DWORD dwStreamSinkIdentifier, IMFStreamSink** ppStreamSink) override {
        UNREFERENCED_PARAMETER(dwStreamSinkIdentifier);
        UNREFERENCED_PARAMETER(ppStreamSink);
        spdlog::trace("{}", __FUNCTION__);
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetPresentationClock(IMFPresentationClock* pPresentationClock) override {
        UNREFERENCED_PARAMETER(pPresentationClock);
        spdlog::trace("{}", __FUNCTION__);
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetPresentationClock(IMFPresentationClock** ppPresentationClock) override {
        UNREFERENCED_PARAMETER(ppPresentationClock);
        spdlog::trace("{}", __FUNCTION__);
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Shutdown() override {
        spdlog::trace("{}", __FUNCTION__);
        return E_NOTIMPL;
    }
};

HRESULT create_sink(const fs::path& dirpath, IMFMediaSink** ptr) {
    if (fs::exists(dirpath) == false || fs::is_directory(dirpath) == false)
        return E_INVALIDARG;
    if (ptr == nullptr)
        return E_INVALIDARG;

    auto report_failure = [](HRESULT code, string&& message) {
        spdlog::error("create_sink: {}", message);
        return code;
    };

    try {
        if (IUnknown* unknown = *ptr = new (nothrow) save_image_sink_t{})
            unknown->AddRef();
        return S_OK;
    } catch (const winrt::hresult_error& ex) {
        return report_failure(ex.code(), winrt::to_string(ex.message()));
    } catch (...) {
        return report_failure(E_FAIL, "unknown");
    }
}
