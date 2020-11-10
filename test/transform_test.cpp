#define CATCH_CONFIG_WINDOWS_CRTDBG
#include <catch2/catch.hpp>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.Threading.h>

#include <filesystem>
#include <media.hpp>

#include <Codecapi.h>
#include <mmdeviceapi.h>
#include <wmsdkidl.h>

#include <spdlog/spdlog.h>

using namespace std;
namespace fs = std::filesystem;

fs::path get_asset_dir() noexcept;

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

    for (ComPtr<IMFActivate> device : devices) {
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
        print(device.Get());
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
        FAIL("test not implemented");
    }
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

// see https://docs.microsoft.com/en-us/windows/win32/medfound/h-264-video-decoder
// see https://docs.microsoft.com/en-us/windows/win32/medfound/basic-mft-processing-model
TEST_CASE("MFTransform - H.264 Decoder", "[codec]") {
    auto on_return = media_startup();

    ComPtr<IMFMediaSourceEx> source{};
    MF_OBJECT_TYPE media_object_type = MF_OBJECT_INVALID;
    REQUIRE(resolve(get_asset_dir() / "fm5p7flyCSY.mp4", source.GetAddressOf(), media_object_type) == S_OK);

    ComPtr<IMFSourceReader> source_reader{};
    {
        ComPtr<IMFAttributes> attrs{};
        REQUIRE(MFCreateAttributes(attrs.GetAddressOf(), 2) == S_OK);
        REQUIRE(attrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE) == S_OK);
        REQUIRE(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE) == S_OK);
        if (auto hr = MFCreateSourceReaderFromMediaSource(source.Get(), attrs.Get(), source_reader.GetAddressOf()))
            FAIL(hr);
    }

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
    const DWORD istream_id = 0;
    const DWORD ostream_id = 0;
    {
        if constexpr (false) {
            ComPtr<IMFMediaType> input{};
            REQUIRE(MFCreateMediaType(input.GetAddressOf()) == S_OK);
            REQUIRE(video_media_type->CopyAllItems(input.Get()) == S_OK);
            if (auto hr = transform->SetInputType(istream_id, input.Get(), 0))
                FAIL(hr);
        } else {
            if (auto hr = transform->SetInputType(istream_id, video_media_type.Get(), 0))
                FAIL(hr);
        }

        ComPtr<IMFMediaType> output{};
        REQUIRE(MFCreateMediaType(output.GetAddressOf()) == S_OK);
        REQUIRE(video_media_type->CopyAllItems(output.Get()) == S_OK);

        if (auto hr = output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_IYUV))
            FAIL(hr);
        if (auto hr = transform->SetOutputType(ostream_id, output.Get(), 0))
            FAIL(hr);

        DWORD status = 0;
        REQUIRE(transform->GetInputStatus(istream_id, &status) == S_OK);
        REQUIRE(status == MFT_INPUT_STATUS_ACCEPT_DATA);
    }
    // for Asynchronous MFT
    REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL) == S_OK);

    // @todo https://docs.microsoft.com/en-us/windows/win32/medfound/basic-mft-processing-model#get-buffer-requirements

    // @see https://docs.microsoft.com/en-us/windows/win32/medfound/basic-mft-processing-model#process-data
    // all types are configured. prepare for upcoming processing
    REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL) == S_OK);

    SECTION("Synchronous Transform - 1") {
        print(transform.Get());

        MFT_INPUT_STREAM_INFO input_stream_info{};
        if (auto hr = transform->GetInputStreamInfo(istream_id, &input_stream_info))
            FAIL(hr);
        MFT_OUTPUT_STREAM_INFO output_stream_info{};
        if (auto hr = transform->GetOutputStreamInfo(ostream_id, &output_stream_info))
            FAIL(hr);

        ComPtr<IMFMediaType> output_type{};
        if (auto hr = transform->GetOutputCurrentType(ostream_id, output_type.GetAddressOf()))
            FAIL(hr);

        size_t count = 0;
        DWORD index{};
        DWORD flags{};
        LONGLONG timestamp{}; // unit 100-nanosecond
        LONGLONG duration{};
        for (ComPtr<IMFSample> input_sample : read_samples(source_reader, //
                                                           index, flags, timestamp, duration)) {
            switch (auto hr = transform->ProcessInput(istream_id, input_sample.Get(), 0)) {
            case S_OK: // MF_E_TRANSFORM_TYPE_NOT_SET, MF_E_NO_SAMPLE_DURATION, MF_E_NO_SAMPLE_TIMESTAMP
                break;
            case MF_E_NOTACCEPTING:
            case MF_E_UNSUPPORTED_D3D_TYPE:
            case E_INVALIDARG:
            default:
                FAIL(hr);
            }
            for (ComPtr<IMFSample> output_sample : decode(transform, output_type, ostream_id)) {
                if (auto hr = check_sample(output_sample))
                    FAIL(hr);
                ++count;
            }
        }
        REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, NULL) == S_OK);
        REQUIRE(transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, NULL) == S_OK);
        REQUIRE(count);
        count = 0;
        // fetch remaining output in the transform
        for (ComPtr<IMFSample> output_sample : decode(transform, output_type, ostream_id)) {
            if (auto hr = check_sample(output_sample))
                FAIL(hr);
            ++count;
        }
        REQUIRE(count);
    }

    SECTION("Synchronous Transform - 2") {
        bool input_available = true;
        while (input_available) {
            DWORD stream_index{};
            DWORD sample_flags{};
            LONGLONG sample_timestamp = 0; // unit 100-nanosecond
            LONGLONG sample_duration = 0;
            ComPtr<IMFSample> input_sample{};
            if (auto hr = source_reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream_index,
                                                    &sample_flags, &sample_timestamp, input_sample.GetAddressOf())) {
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

            constexpr DWORD istream = 0;
            switch (auto hr = transform->ProcessInput(istream, input_sample.Get(), 0)) {
            case S_OK: // MF_E_TRANSFORM_TYPE_NOT_SET, MF_E_NO_SAMPLE_DURATION, MF_E_NO_SAMPLE_TIMESTAMP
                break;
            case MF_E_NOTACCEPTING:
            case MF_E_UNSUPPORTED_D3D_TYPE:
            case E_INVALIDARG:
            default:
                FAIL(hr);
            }

            MFT_OUTPUT_DATA_BUFFER output{};
            MFT_OUTPUT_STREAM_INFO stream{};
            output.dwStreamID = 0;
            while (true) {
                DWORD status = 0; // MFT_OUTPUT_STATUS_SAMPLE_READY
                if (auto hr = transform->GetOutputStreamInfo(output.dwStreamID, &stream))
                    FAIL(hr);

                ComPtr<IMFSample> output_sample{};
                if (stream.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) {
                    // ...
                } else {
                    if (auto hr = create_single_buffer_sample(stream.cbSize, output_sample.GetAddressOf()))
                        FAIL(hr);
                    output.pSample = output_sample.Get();
                }
                auto hr = transform->ProcessOutput(0, 1, &output, &status);
                if (hr == S_OK)
                    continue;
                if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
                    break;
                if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                    ComPtr<IMFMediaType> output_type{};
                    if (output.dwStatus != MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE) {
                        // todo: add more works for this case
                        FAIL("MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE");
                    }
                    // ...
                    if (auto hr = transform->GetOutputAvailableType(output.dwStreamID, 0, output_type.GetAddressOf()))
                        FAIL(hr);
                    // specify the format we want ...
                    if (auto hr = output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_IYUV))
                        FAIL(hr);
                    if (auto hr = transform->SetOutputType(0, output_type.Get(), 0))
                        FAIL(hr);
                    continue;
                }
                FAIL(hr);
            }
        }
        REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, NULL) == S_OK);
        REQUIRE(transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, NULL) == S_OK);
        // fetch remaining output in the transform
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
            // wcerr << ex.ErrorMessage() << endl;
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
