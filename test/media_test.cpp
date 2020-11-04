#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_WINDOWS_CRTDBG
#include <catch2/catch.hpp>

#include <filesystem>
#include <media.hpp>

#include <Codecapi.h>
#include <mmdeviceapi.h>
#include <wmsdkidl.h>

using namespace std;
namespace fs = std::filesystem;

fs::path get_asset_dir() noexcept {
#if defined(ASSET_DIR)
    return {ASSET_DIR};
#else
    return fs::current_path();
#endif
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
    qpc_timer_t timer{};
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
        if (timer.pick() >= 2'000)
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

[[deprecated]] HRESULT configure(ComPtr<IMFSourceReader> reader, DWORD stream) noexcept {
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

// there might be no device in test environment. if the case, it's an expected failure
TEST_CASE("IMFActivate(IMFMediaSourceEx)", "[!mayfail]") {
    auto on_return = media_startup();

    std::vector<ComPtr<IMFActivate>> devices{};
    {
        ComPtr<IMFAttributes> attrs{};
        REQUIRE(MFCreateAttributes(attrs.GetAddressOf(), 1) == S_OK);
        REQUIRE(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, //
                               MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID) == S_OK);
        REQUIRE(get_devices(attrs.Get(), devices) == S_OK);
    }
    REQUIRE(devices.size());

    for (auto device : devices) {
        std::wstring name{};
        REQUIRE(get_name(device.Get(), name) == S_OK);
        wcout << name << endl;
        fwprintf_s(stdout, L"- device:\n");
        fwprintf_s(stdout, L"  - name: %s\n", name.c_str());

        ComPtr<IMFMediaSourceEx> source{};
        if (auto hr = device->ActivateObject(__uuidof(IMFMediaSourceEx), (void**)source.GetAddressOf()))
            FAIL(hr);

        ComPtr<IMFAttributes> attributes{};
        if (auto hr = source->GetSourceAttributes(attributes.GetAddressOf()))
            FAIL(hr);

        GUID capture{};
        if (auto hr = attributes->GetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &capture))
            FAIL(hr);
        REQUIRE(capture == MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        // REQUIRE(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE) == S_OK);

        // print more detail ...?
    }
}

TEST_CASE("IMFMediaSession(Unprotected)") {
    auto on_return = media_startup();

    ComPtr<IMFAttributes> session_attributes{};
    ComPtr<IMFMediaSession> session{};
    REQUIRE(MFCreateMediaSession(session_attributes.Get(), session.GetAddressOf()) == S_OK);

    SECTION("close without start") {
        REQUIRE(session->Close() == S_OK);
    }
}

TEST_CASE("IMFSourceResolver") {
    auto on_return = media_startup();

    const auto fpath = fs::absolute(get_asset_dir() / "fm5p7flyCSY.mp4");
    REQUIRE(PathFileExistsW(fpath.c_str()));

    ComPtr<IMFSourceResolver> resolver{};
    if (auto hr = MFCreateSourceResolver(resolver.GetAddressOf()))
        FAIL(hr);

    ComPtr<IUnknown> unknown{};
    MF_OBJECT_TYPE media_object_type{};
    if (auto hr = resolver->CreateObjectFromURL(fpath.c_str(), MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_READ, NULL,
                                                &media_object_type, unknown.GetAddressOf()))
        FAIL(hr); // MF_E_UNSUPPORTED_SCHEME, MF_E_SOURCERESOLVER_MUTUALLY_EXCLUSIVE_FLAGS
    REQUIRE(media_object_type == MF_OBJECT_MEDIASOURCE);

    SECTION("IMFMediaSource") {
        ComPtr<IMFMediaSource> source{};
        REQUIRE(unknown->QueryInterface(IID_PPV_ARGS(source.GetAddressOf())) == S_OK);
        // consume source ...
    }
    SECTION("IMFMediaSourceEx") {
        ComPtr<IMFMediaSourceEx> source{};
        REQUIRE(unknown->QueryInterface(IID_PPV_ARGS(source.GetAddressOf())) == S_OK);
        // consume source ...
    }
}

TEST_CASE("MFCreateSourceReader", "[!mayfail]") {
    auto on_return = media_startup();

    ComPtr<IMFAttributes> attributes{};
    REQUIRE(MFCreateAttributes(attributes.GetAddressOf(), 2) == S_OK);

    const auto fpath = get_asset_dir() / "fm5p7flyCSY.mp4";
    SECTION("URL") {
        ComPtr<IMFSourceReader> reader{};
        REQUIRE(MFCreateSourceReaderFromURL(fpath.c_str(), nullptr, reader.GetAddressOf()) == S_OK);
        ComPtr<IMFSourceReaderEx> reader2{};
        REQUIRE(reader->QueryInterface(reader2.GetAddressOf()) == S_OK);
    }
    SECTION("IMFMediaSource") {
        ComPtr<IMFMediaSourceEx> source{};
        MF_OBJECT_TYPE media_object_type = MF_OBJECT_INVALID;
        REQUIRE(resolve(fpath, source.GetAddressOf(), media_object_type) == S_OK);

        ComPtr<IMFSourceReader> reader{};
        REQUIRE(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, reader.GetAddressOf()) == S_OK);
    }
    SECTION("IMFByteStream") {
        FAIL("not implemented");
    }
}

// see https://docs.microsoft.com/en-us/windows/win32/medfound/h-264-video-decoder
// see https://docs.microsoft.com/en-us/windows/win32/medfound/basic-mft-processing-model
TEST_CASE("MFTransform - H.264 Video Decoder", "[codec]") {
    auto on_return = media_startup();

    ComPtr<IMFMediaSourceEx> source{};
    MF_OBJECT_TYPE media_object_type = MF_OBJECT_INVALID;
    REQUIRE(resolve(get_asset_dir() / "fm5p7flyCSY.mp4", source.GetAddressOf(), media_object_type) == S_OK);

    ComPtr<IMFSourceReader> source_reader{};
    REQUIRE(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, source_reader.GetAddressOf()) == S_OK);

    ComPtr<IMFMediaType> video_media_type{};
    REQUIRE(source_reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                               video_media_type.GetAddressOf()) == S_OK);

    ComPtr<IMFTransform> transform{};
    REQUIRE(make_transform_H264(transform.GetAddressOf()) == S_OK);
    {
        // https://docs.microsoft.com/en-us/windows/win32/medfound/h-264-video-decoder#transform-attributes
        ComPtr<IMFAttributes> attributes{};
        REQUIRE(transform->GetAttributes(attributes.GetAddressOf()) == S_OK);
        CAPTURE(attributes->SetUINT32(CODECAPI_AVDecVideoAcceleration_H264, TRUE)); // Codecapi.h
        CAPTURE(attributes->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE));
        CAPTURE(attributes->SetUINT32(CODECAPI_AVDecNumWorkerThreads, 1));
    }

    // Valid configuration order can be I->O or O->I. `CLSID_CMSH264DecoderMFT` uses I->O ordering
    {
        const DWORD istream = 0;
        if constexpr (false) {
            ComPtr<IMFMediaType> input{};
            REQUIRE(MFCreateMediaType(input.GetAddressOf()) == S_OK);
            REQUIRE(video_media_type->CopyAllItems(input.Get()) == S_OK);
            if (auto hr = transform->SetInputType(istream, input.Get(), 0))
                FAIL(hr);
        } else {
            if (auto hr = transform->SetInputType(istream, video_media_type.Get(), 0))
                FAIL(hr);
        }

        ComPtr<IMFMediaType> output{};
        REQUIRE(MFCreateMediaType(output.GetAddressOf()) == S_OK);
        REQUIRE(video_media_type->CopyAllItems(output.Get()) == S_OK);

        const DWORD ostream = 0;
        SECTION("MFVideoFormat_IYUV") {
            if (auto hr = output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_IYUV))
                FAIL(hr);
            if (auto hr = transform->SetOutputType(ostream, output.Get(), 0))
                FAIL(hr);
        }
        SECTION("MFVideoFormat_I420") {
            if (auto hr = output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_I420))
                FAIL(hr);
            if (auto hr = transform->SetOutputType(ostream, output.Get(), 0))
                FAIL(hr);
        }
        DWORD status = 0;
        REQUIRE(transform->GetInputStatus(0, &status) == S_OK);
        REQUIRE(status == MFT_INPUT_STATUS_ACCEPT_DATA);
    }
    // for Asynchronous MFT
    REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL) == S_OK);

    // @todo https://docs.microsoft.com/en-us/windows/win32/medfound/basic-mft-processing-model#get-buffer-requirements

    // all types are configured. prepare for upcoming processing
    REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL) == S_OK);

    /// @todo support `MF_E_TRANSFORM_STREAM_CHANGE`
    auto consume_output_sample = [](IMFTransform* transform) -> HRESULT {
        ComPtr<IMFSample> sample{};
        if (auto hr = MFCreateSample(sample.GetAddressOf()))
            return hr;

        constexpr DWORD stream_id = 0;
        MFT_OUTPUT_STREAM_INFO stream{};
        if (auto hr = transform->GetOutputStreamInfo(stream_id, &stream))
            return hr;

        ComPtr<IMFMediaBuffer> buffer{};
        if (auto hr = MFCreateMemoryBuffer(stream.cbSize, buffer.GetAddressOf()))
            return hr;
        if (auto hr = sample->AddBuffer(buffer.Get()))
            return hr;

        MFT_OUTPUT_DATA_BUFFER output{};
        DWORD status = 0;
        return transform->ProcessOutput(0, 1, &output, &status);
    };

    size_t count = 0;
    bool input_available = true;
    while (input_available) {
        DWORD stream_index{};
        ComPtr<IMFSample> input_sample{};
        DWORD sample_flags{};
        LONGLONG sample_timestamp = 0; // unit 100-nanosecond
        LONGLONG sample_duration = 0;
        if (auto hr = source_reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream_index, &sample_flags,
                                                &sample_timestamp, input_sample.GetAddressOf())) {
            CAPTURE(sample_flags);
            FAIL(hr);
        }
        if (sample_flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            input_available = false;
            continue;
        }
        // probably MF_SOURCE_READERF_STREAMTICK
        if (input_sample == nullptr)
            continue;

        ComPtr<IMFSample> copied_sample{};
        if (auto hr = create_and_copy_single_buffer_sample(input_sample.Get(), copied_sample.GetAddressOf()))
            FAIL(hr);
        copied_sample->SetSampleDuration(sample_duration);
        copied_sample->SetSampleTime(sample_timestamp);

        constexpr DWORD istream = 0;
        switch (auto hr = transform->ProcessInput(istream, copied_sample.Get(), 0)) {
        case S_OK: // MF_E_TRANSFORM_TYPE_NOT_SET, MF_E_NO_SAMPLE_DURATION, MF_E_NO_SAMPLE_TIMESTAMP
            break;
        case MF_E_NOTACCEPTING:
        case MF_E_UNSUPPORTED_D3D_TYPE:
        case E_INVALIDARG:
        default:
            FAIL(hr);
        }

        DWORD status = 0;
        if (auto hr = transform->GetOutputStatus(&status))
            FAIL(hr);
        if (status & MFT_OUTPUT_STATUS_SAMPLE_READY) {
            switch (auto hr = consume_output_sample(transform.Get())) {
            case MF_E_TRANSFORM_NEED_MORE_INPUT:
            case S_OK:
                continue;
            default:
                FAIL(hr);
            }
        }
    }
    REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, NULL) == S_OK);
    REQUIRE(transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, NULL) == S_OK);
    // fetch remaining output in the transform
    while (true) {
        auto hr = consume_output_sample(transform.Get());
        if (hr == S_OK)
            continue;
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
            break;
        FAIL(hr);
    }
    REQUIRE(count);
}

HRESULT check_sample(ComPtr<IMFSample> sample) {
    ComPtr<IMFMediaBuffer> buffer{};
    if (auto hr = sample->ConvertToContiguousBuffer(buffer.GetAddressOf()))
        return hr;
    DWORD bufsz{};
    if (auto hr = buffer->GetCurrentLength(&bufsz))
        return hr;

    BYTE* ptr = NULL;
    DWORD capacity = 0, length = 0;
    if (auto hr = buffer->Lock(&ptr, &capacity, &length))
        return hr;
    auto on_return = gsl::finally([buffer]() { buffer->Unlock(); });

    // consume the IMFSample
    return S_OK;
}
