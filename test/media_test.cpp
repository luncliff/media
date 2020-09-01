#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_WINDOWS_CRTDBG
#include <catch2/catch.hpp>

#include <filesystem>
#include <media.hpp>

using namespace std;
namespace fs = std::filesystem;

fs::path get_asset_dir() noexcept {
#if defined(ASSET_DIR)
    return {ASSET_DIR};
#else
    return fs::current_path();
#endif
}

TEST_CASE("get_devices") {
    auto on_return = startup();
    ComPtr<IMFAttributes> attrs{};
    HRESULT hr = MFCreateAttributes(attrs.GetAddressOf(), 1);
    REQUIRE_FALSE(FAILED(hr));
    hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    REQUIRE_FALSE(FAILED(hr));

    std::vector<ComPtr<IMFActivate>> devices{};
    hr = get_devices(attrs.Get(), devices);
    REQUIRE_FALSE(FAILED(hr));
    for (auto device : devices) {
        std::wstring name{};
        REQUIRE(get_name(device.Get(), name) == S_OK);
    }
}

/// @see OnReadSample
struct read_item_t final {
    HRESULT status;
    DWORD index;
    DWORD flags;
    std::chrono::nanoseconds timestamp; // unit: 100 nanosecond
    ComPtr<IMFSample> sample;
};
static_assert(sizeof(read_item_t) == 32);

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/processing-media-data-with-the-source-reader
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/using-the-source-reader-in-asynchronous-mode
struct reader_impl_t : public IMFSourceReaderCallback {
    ComPtr<IMFSourceReader> reader{};
    process_timer_t timer{};
    SHORT ref_count = 0;

  private:
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent* event) noexcept override {
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
        if (timer.pick() >= 2)
            return S_OK;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            return S_OK;

        if (flags & MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED) {
            // must configure the reader again
        }
        // MF_SOURCE_READERF_ERROR
        // MF_SOURCE_READERF_ENDOFSTREAM
        // MF_SOURCE_READERF_NEWSTREAM
        // MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED
        // MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED
        // MF_SOURCE_READERF_STREAMTICK
        // MF_SOURCE_READERF_ALLEFFECTSREMOVED
        read_item_t item{};
        item.status = status;
        item.index = index;
        item.sample = sample;
        item.flags = flags;
        item.timestamp = std::chrono::nanoseconds{timestamp * 100};
        try {
            // request again
            if (auto hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, NULL, NULL, NULL);
                SUCCEEDED(hr) == false)
                throw _com_error{hr};
            return S_OK;
        } catch (const _com_error& ex) {
            wcerr << ex.ErrorMessage() << endl;
            return ex.Error();
        }
    }

  public:
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) {
        const QITAB qit[] = {
            QITABENT(reader_impl_t, IMFSourceReaderCallback),
            {},
        };
        return QISearch(this, qit, iid, ppv);
    }
    STDMETHODIMP_(ULONG) AddRef() {
        return InterlockedIncrement16(&ref_count);
    }
    STDMETHODIMP_(ULONG) Release() {
        return InterlockedDecrement16(&ref_count);
    }
};

void consume_source(ComPtr<IMFMediaSource> source, IMFSourceReaderCallback* callback, IMFSourceReader** reader_ptr) {
    ComPtr<IMFPresentationDescriptor> presentation{};
    if (auto hr = source->CreatePresentationDescriptor(presentation.GetAddressOf()))
        REQUIRE(SUCCEEDED(hr));
    ComPtr<IMFStreamDescriptor> stream{};
    if (auto hr = get_stream_descriptor(presentation.Get(), stream.GetAddressOf()))
        REQUIRE(SUCCEEDED(hr));
    if (auto hr = configure(stream))
        REQUIRE(SUCCEEDED(hr));

    ComPtr<IMFAttributes> attrs{};
    if (auto hr = MFCreateAttributes(attrs.GetAddressOf(), 1))
        REQUIRE(SUCCEEDED(hr));
    if (auto hr = attrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback))
        REQUIRE(SUCCEEDED(hr));
    if (auto hr = MFCreateSourceReaderFromMediaSource(source.Get(), attrs.Get(), reader_ptr))
        REQUIRE(SUCCEEDED(hr));
}

HRESULT configure(ComPtr<IMFSourceReader> reader, DWORD stream) noexcept {
    // native format of the stream
    ComPtr<IMFMediaType> native = nullptr;
    if (auto hr = reader->GetNativeMediaType(stream, 0, native.GetAddressOf()); FAILED(hr))
        return hr;

    // decoding output
    ComPtr<IMFMediaType> output = nullptr;
    if (auto hr = MFCreateMediaType(output.GetAddressOf()); FAILED(hr))
        return hr;

    // sync major type (video)
    GUID major{};
    if (auto hr = native->GetGUID(MF_MT_MAJOR_TYPE, &major); FAILED(hr))
        return hr;
    if (auto hr = output->SetGUID(MF_MT_MAJOR_TYPE, major); FAILED(hr))
        return hr;

    // subtype configuration (video format)
    GUID subtype{}; // MFVideoFormat_RGB32
    if (auto hr = native->GetGUID(MF_MT_SUBTYPE, &subtype); FAILED(hr))
        return hr;
    if (auto hr = output->SetGUID(MF_MT_SUBTYPE, subtype); FAILED(hr))
        return hr;

    return reader->SetCurrentMediaType(stream, NULL, output.Get());
}

void read_on_background(ComPtr<IMFSourceReader> reader) {
    if (auto hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, NULL, NULL, NULL))
        REQUIRE(SUCCEEDED(hr));
    SleepEx(3 * 1'000, true);
}

TEST_CASE("IMFMediaSource(IMFActivate)") {
    auto on_return = startup();

    ComPtr<IMFAttributes> attrs{};
    std::vector<ComPtr<IMFActivate>> devices{};
    if (auto hr = MFCreateAttributes(attrs.GetAddressOf(), 1)) //
        REQUIRE(SUCCEEDED(hr));
    if (auto hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, //
                                 MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))
        REQUIRE(SUCCEEDED(hr));
    if (auto hr = get_devices(attrs.Get(), devices))
        REQUIRE(SUCCEEDED(hr));
    REQUIRE(devices.size() > 0);

    for (auto device : devices) {
        std::wstring name{};
        REQUIRE(get_name(device.Get(), name) == S_OK);
        wcout << name << endl;
    }

    ComPtr<IMFActivate> device = devices[0];
    ComPtr<IMFMediaSource> source{};
    if (auto hr = device->ActivateObject(__uuidof(IMFMediaSource), (void**)source.GetAddressOf()))
        REQUIRE(SUCCEEDED(hr));

    reader_impl_t impl{};
    consume_source(source, &impl, impl.reader.GetAddressOf());
    configure(impl.reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    read_on_background(impl.reader);
}

TEST_CASE("IMFMediaSource(IMFSourceResolver)") {
    auto on_return = startup();
    ComPtr<IMFAttributes> attrs{};
    ComPtr<IMFMediaSession> session{};
    REQUIRE(MFCreateMediaSession(attrs.Get(), session.GetAddressOf()) == S_OK);

    const auto fpath = fs::absolute(get_asset_dir() / "MOT17-02-SDP-raw.mp4");
    LPCWSTR url = fpath.c_str();
    REQUIRE(PathFileExistsW(url));
    wcout << url << endl;

    ComPtr<IMFSourceResolver> resolver{};
    if (auto hr = MFCreateSourceResolver(resolver.GetAddressOf()); FAILED(hr))
        FAIL(hr);
    ComPtr<IUnknown> ptr{};
    MF_OBJECT_TYPE type{};
    if (auto hr = resolver->CreateObjectFromURL(url, MF_RESOLUTION_MEDIASOURCE, NULL, &type, ptr.GetAddressOf());
        FAILED(hr))
        FAIL(hr);
    ComPtr<IMFMediaSource> source{};
    REQUIRE(ptr->QueryInterface(IID_PPV_ARGS(source.GetAddressOf())) == S_OK);

    reader_impl_t impl{};
    consume_source(source, &impl, impl.reader.GetAddressOf());
    configure(impl.reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    read_on_background(impl.reader);
}
