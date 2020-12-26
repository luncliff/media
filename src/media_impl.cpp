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

void print(const winrt::hresult_error& ex) noexcept {
    spdlog::error("hresult {:#x}: {}", static_cast<uint32_t>(ex.code()), winrt::to_string(ex.message()));
}
void print(const std::exception& ex) noexcept {
    spdlog::error(ex.what());
}

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/processing-media-data-with-the-source-reader
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/using-the-source-reader-in-asynchronous-mode
/// @see https://docs.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsourcereadercallback-onreadsample
/// @see https://docs.microsoft.com/en-us/windows/win32/api/mfreadwrite/ne-mfreadwrite-mf_source_reader_flag
class verbose_callback_t final : public IMFSourceReaderCallback {
    critical_section_t mtx{};
    com_ptr<IMFSourceReader> reader{};
    qpc_timer_t timer{};
    com_ptr<IMFPresentationClock> clock{};
    com_ptr<IMFPresentationTimeSource> time_source{};
    SHORT ref_count = 0;

  public:
    verbose_callback_t() noexcept(false) {
        winrt::check_hresult(MFCreatePresentationClock(clock.put()));
        winrt::check_hresult(MFCreateSystemTimeSource(time_source.put()));
        winrt::check_hresult(clock->SetTimeSource(time_source.get()));
    }

  private:
    STDMETHODIMP OnEvent(DWORD stream, IMFMediaEvent* event) noexcept override {
        UNREFERENCED_PARAMETER(stream);
        UNREFERENCED_PARAMETER(event);
        spdlog::trace("{}", __FUNCTION__);
        lock_guard lck{mtx};
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD stream) noexcept override {
        UNREFERENCED_PARAMETER(stream);
        spdlog::trace("{}", __FUNCTION__);
        lock_guard lck{mtx};
        reader = nullptr;
        return S_OK;
    }
    STDMETHODIMP OnReadSample(HRESULT status, DWORD stream, DWORD flags, //
                              LONGLONG timestamp, IMFSample* sample) noexcept override {
        spdlog::trace("{}: {:#x}", __FUNCTION__, flags);
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
            spdlog::trace("  buflen: {}", buflen);
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

    if (ptr == nullptr)
        return E_INVALIDARG;
    try {
        if (IUnknown* unknown = *ptr = new (nothrow) verbose_callback_t{})
            unknown->AddRef();
        return S_OK;
    } catch (const winrt::hresult_error& ex) {
        print(ex);
        return ex.code();
    } catch (const std::exception& ex) {
        print(ex);
        return E_FAIL;
    }
}

class save_image_sink_t final : public IMFMediaSink {
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

    try {
        if (IUnknown* unknown = *ptr = new (nothrow) save_image_sink_t{})
            unknown->AddRef();
        return S_OK;
    } catch (const winrt::hresult_error& ex) {
        print(ex);
        return ex.code();
    } catch (const std::exception& ex) {
        print(ex);
        return E_FAIL;
    }
}

h264_video_writer_t::h264_video_writer_t(const fs::path& fpath) noexcept(false) {
    winrt::check_hresult(create_sink_writer(writer.put(), fpath));
    winrt::check_hresult(MFCreateMediaType(output_type.put()));
    winrt::check_hresult(output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    winrt::check_hresult(output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
    winrt::check_hresult(output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
}
h264_video_writer_t::~h264_video_writer_t() noexcept {
    try {
        winrt::check_hresult(writer->Finalize());
    } catch (const winrt::hresult_error& ex) {
        print(ex);
    }
}

HRESULT h264_video_writer_t::use_source(com_ptr<IMFMediaType> input_type) noexcept {
    if (input_type == nullptr)
        return E_INVALIDARG;

    UINT32 fps_num = 0, fps_denom = 1;
    if (auto hr = MFGetAttributeRatio(input_type.get(), MF_MT_FRAME_RATE, &fps_num, &fps_denom); FAILED(hr))
        return hr;
    if (auto hr = MFSetAttributeRatio(output_type.get(), MF_MT_FRAME_RATE, fps_num, fps_denom); FAILED(hr))
        return hr;

    UINT32 width = 0, height = 0;
    if (auto hr = MFGetAttributeSize(input_type.get(), MF_MT_FRAME_SIZE, &width, &height); FAILED(hr))
        return hr;
    if (auto hr = MFSetAttributeSize(output_type.get(), MF_MT_FRAME_SIZE, width, height); FAILED(hr))
        return hr;

    const UINT32 bitrate = 800'000;
    if (auto hr = output_type->SetUINT32(MF_MT_AVG_BITRATE, bitrate); FAILED(hr))
        return hr;

    if (auto hr = writer->AddStream(output_type.get(), &stream_index); FAILED(hr))
        return hr;
    if (auto hr = writer->SetInputMediaType(stream_index, input_type.get(), NULL); FAILED(hr))
        return hr;
    return S_OK;
}

HRESULT h264_video_writer_t::begin() noexcept {
    return writer->BeginWriting();
}

HRESULT h264_video_writer_t::write(IMFSample* sample) noexcept {
    if (sample == nullptr)
        return E_INVALIDARG;
    return writer->WriteSample(stream_index, sample);
}

capture_session_t::capture_session_t(com_ptr<IMFActivate> device) noexcept(false) {
    winrt::check_hresult(device->ActivateObject(__uuidof(IMFMediaSourceEx), source.put_void()));
}

capture_session_t::~capture_session_t() noexcept {
    try {
        winrt::check_hresult(source->Shutdown());
    } catch (const winrt::hresult_error& ex) {
        print(ex);
    }
}

HRESULT capture_session_t::open(com_ptr<IMFSourceReaderCallback> callback, DWORD stream) noexcept {
    if (reader != nullptr)
        return E_NOT_VALID_STATE;
    reader_callback = callback;
    reader_stream = stream;
    com_ptr<IMFSourceReader> source_reader{};
    if (auto hr = create_source_reader(source, reader_callback, source_reader.put()); FAILED(hr))
        return hr;
    if (auto hr = source_reader->QueryInterface(reader.put()); FAILED(hr))
        return hr;
    return reader->GetNativeMediaType(reader_stream, 0, source_type.put());
}

com_ptr<IMFSourceReader> capture_session_t::get_reader() const noexcept {
    return reader;
}
com_ptr<IMFMediaType> capture_session_t::get_source_type() const noexcept {
    return source_type;
}
