#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_WINDOWS_CRTDBG
#include <catch2/catch.hpp>

#include <media.hpp>

using namespace std;

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

struct reader_context_t : public IMFSourceReaderCallback {
    ComPtr<IMFSourceReader> reader{};
    process_timer_t timer{};

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

  private:
    SHORT ref_count = 0;

  public:
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) {
        const QITAB qit[] = {
            QITABENT(reader_context_t, IMFSourceReaderCallback),
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

void consume_source(ComPtr<IMFMediaSource> source) {
    ComPtr<IMFPresentationDescriptor> presentation{};
    if (auto hr = source->CreatePresentationDescriptor(presentation.GetAddressOf()))
        REQUIRE(SUCCEEDED(hr));
    ComPtr<IMFStreamDescriptor> stream{};
    if (auto hr = get_stream_descriptor(presentation.Get(), stream.GetAddressOf()))
        REQUIRE(SUCCEEDED(hr));
    if (auto hr = configure(stream))
        REQUIRE(SUCCEEDED(hr));

    reader_context_t context{};
    ComPtr<IMFAttributes> attrs{};
    if (auto hr = MFCreateAttributes(attrs.GetAddressOf(), 1))
        REQUIRE(SUCCEEDED(hr));
    IMFSourceReaderCallback* callback = &context;
    if (auto hr = attrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback))
        REQUIRE(SUCCEEDED(hr));
    ComPtr<IMFSourceReader> reader{};
    if (auto hr = MFCreateSourceReaderFromMediaSource(source.Get(), attrs.Get(), //
                                                      reader.GetAddressOf()))
        REQUIRE(SUCCEEDED(hr));
    context.reader = reader;
    if (auto hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, NULL, NULL, NULL))
        REQUIRE(SUCCEEDED(hr));
    SleepEx(3 * 1'000, true);
    REQUIRE(reader);
}

TEST_CASE("IMFMediaSource(IMFActivate)") {
    auto on_return = startup();
    device_group_t group{};
    REQUIRE(group.devices.size() > 0);

    for (auto device : group.devices) {
        std::wstring name{};
        REQUIRE(get_name(device.Get(), name) == S_OK);
        wcout << name << endl;
    }

    ComPtr<IMFActivate> device = group.devices.front();
    ComPtr<IMFMediaSource> source{};
    if (auto hr = device->ActivateObject(__uuidof(IMFMediaSource), (void**)source.GetAddressOf()))
        REQUIRE(SUCCEEDED(hr));

    return consume_source(source);
}

TEST_CASE("IMFMediaSource(IMFSourceResolver)") {
    auto on_return = startup();
    ComPtr<IMFAttributes> attrs{};
    ComPtr<IMFMediaSession> session{};
    REQUIRE(MFCreateMediaSession(attrs.Get(), session.GetAddressOf()) == S_OK);

    LPCWSTR url = L"C:\\path\\to\\test.mp4";
    REQUIRE(PathFileExistsW(url));

    ComPtr<IMFSourceResolver> resolver{};
    if (auto hr = MFCreateSourceResolver(resolver.GetAddressOf()); FAILED(hr))
        FAIL(hr);
    ComPtr<IUnknown> proxy{};
    MF_OBJECT_TYPE type{};
    if (auto hr = resolver->CreateObjectFromURL(url, MF_RESOLUTION_MEDIASOURCE, NULL, &type, proxy.GetAddressOf());
        FAILED(hr))
        FAIL(hr);
    ComPtr<IMFMediaSource> source{};
    REQUIRE(proxy->QueryInterface(IID_PPV_ARGS(source.GetAddressOf())) == S_OK);

    return consume_source(source);
}
