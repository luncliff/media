#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_WINDOWS_CRTDBG
#include <catch2/catch.hpp>

#include <filesystem>
#include <media.hpp>

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

TEST_CASE("get_devices") {
    auto on_return = media_startup();
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

TEST_CASE("IMFMediaSource(IMFActivate)", "[!mayfail]") {
    auto on_return = media_startup();

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
    if (auto hr = impl.reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, NULL, NULL, NULL))
        REQUIRE(SUCCEEDED(hr));
    SleepEx(3 * 1'000, true);
}

TEST_CASE("IMFMediaSource(IMFSourceResolver)") {
    auto on_return = media_startup();
    ComPtr<IMFAttributes> attrs{};
    ComPtr<IMFMediaSession> session{};
    REQUIRE(MFCreateMediaSession(attrs.Get(), session.GetAddressOf()) == S_OK);

    const auto fpath = fs::absolute(get_asset_dir() / "fm5p7flyCSY.mp4");
    LPCWSTR url = fpath.c_str();
    REQUIRE(PathFileExistsW(url));

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
    if (auto hr = impl.reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, NULL, NULL, NULL))
        REQUIRE(SUCCEEDED(hr));
    SleepEx(3 * 1'000, true);
}

TEST_CASE("MFTransform(MP4-YUV)") {
    const auto fpath = fs::absolute(get_asset_dir() / "fm5p7flyCSY.mp4");

    auto on_return = media_startup();

    MF_OBJECT_TYPE ObjectType = MF_OBJECT_INVALID;

    ComPtr<IMFSourceResolver> resolver{};
    ComPtr<IUnknown> source{};
    REQUIRE(MFCreateSourceResolver(resolver.GetAddressOf()) == S_OK);
    REQUIRE(resolver->CreateObjectFromURL(fpath.c_str(),             // URL of the source.
                                          MF_RESOLUTION_MEDIASOURCE, // Create a source object.
                                          NULL,                      // Optional property store.
                                          &ObjectType,               // Receives the created object type.
                                          source.GetAddressOf()      // Receives a pointer to the media source.
                                          ) == S_OK);

    ComPtr<IMFMediaSource> media_source{};
    REQUIRE(source->QueryInterface(IID_PPV_ARGS(media_source.GetAddressOf())) == S_OK);

    ComPtr<IMFAttributes> reader_attributes{};
    REQUIRE(MFCreateAttributes(reader_attributes.GetAddressOf(), 2) == S_OK);
    REQUIRE(reader_attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                       MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID) == S_OK);
    REQUIRE(reader_attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, 1) == S_OK);

    ComPtr<IMFSourceReader> source_reader{};
    REQUIRE(MFCreateSourceReaderFromMediaSource(media_source.Get(), reader_attributes.Get(),
                                                source_reader.GetAddressOf()) == S_OK);

    ComPtr<IMFMediaType> file_video_media_type{};
    REQUIRE(source_reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                               file_video_media_type.GetAddressOf()) == S_OK);
    {
        GUID major{};
        REQUIRE(file_video_media_type->GetMajorType(&major) == S_OK);
        CAPTURE(get_name(major));
        REQUIRE(major == MFMediaType_Video);
    }

    // Create H.264 decoder.
    ComPtr<IUnknown> transform{};
    ComPtr<IMFTransform> decoding_transform{}; // This is H264 Decoder MFT
    REQUIRE(CoCreateInstance(CLSID_CMSH264DecoderMFT, NULL, CLSCTX_INPROC_SERVER, IID_IUnknown,
                             (void**)transform.GetAddressOf()) == S_OK);
    REQUIRE(transform->QueryInterface(IID_PPV_ARGS(decoding_transform.GetAddressOf())) == S_OK);

    ComPtr<IMFMediaType> input_media_type{};
    REQUIRE(MFCreateMediaType(input_media_type.GetAddressOf()) == S_OK);
    REQUIRE(file_video_media_type->CopyAllItems(input_media_type.Get()) == S_OK);
    if (auto hr = decoding_transform->SetInputType(0, input_media_type.Get(), 0))
        FAIL(hr);

    ComPtr<IMFMediaType> output_media_type{};
    REQUIRE(MFCreateMediaType(output_media_type.GetAddressOf()) == S_OK);
    REQUIRE(file_video_media_type->CopyAllItems(output_media_type.Get()) == S_OK);
    if (auto hr = output_media_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_IYUV))
        FAIL(hr);

    if (auto hr = decoding_transform->SetOutputType(0, output_media_type.Get(), 0))
        FAIL(hr);

    DWORD status = 0;
    REQUIRE(decoding_transform->GetInputStatus(0, &status) == S_OK);
    REQUIRE(status == MFT_INPUT_STATUS_ACCEPT_DATA);
    //CAPTURE(GetMediaTypeDescription(output_media_type.Get()));
    REQUIRE(decoding_transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL) == S_OK);
    REQUIRE(decoding_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL) == S_OK);
    REQUIRE(decoding_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL) == S_OK);

    int count = 0;
    while (true) {
        ComPtr<IMFSample> video_sample{};
        DWORD stream_index{};
        DWORD flags{};
        LONGLONG sample_timestamp = 0; // unit 100-nanosecond
        LONGLONG sample_duration = 0;
        if (auto hr = source_reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream_index, &flags,
                                                &sample_timestamp, video_sample.GetAddressOf()))
            FAIL(hr);

        if (flags & MF_SOURCE_READERF_STREAMTICK) {
            INFO("MF_SOURCE_READERF_STREAMTICK");
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            INFO("MF_SOURCE_READERF_ENDOFSTREAM");
            break;
        }
        if (flags & MF_SOURCE_READERF_NEWSTREAM) {
            INFO("MF_SOURCE_READERF_NEWSTREAM");
            break;
        }
        if (flags & MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED) {
            INFO("MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED");
            break;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            INFO("MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED");
            break;
        }

        if (video_sample) {
            if (auto hr = video_sample->SetSampleTime(sample_timestamp))
                FAIL(hr);
            if (auto hr = video_sample->GetSampleDuration(&sample_duration))
                FAIL(hr);
            DWORD flags = 0;
            if (auto hr = video_sample->GetSampleFlags(&flags))
                FAIL(hr);

            // Replicate transmitting the sample across the network and reconstructing.
            ComPtr<IMFSample> copied_sample{};
            if (auto hr = create_and_copy_single_buffer_sample(video_sample.Get(), copied_sample.GetAddressOf()))
                FAIL(hr);

            // Apply the H264 decoder transform
            if (auto hr = decoding_transform->ProcessInput(0, copied_sample.Get(), 0))
                FAIL(hr);

            HRESULT result = S_OK;
            while (result == S_OK) {
                ComPtr<IMFSample> decoded_sample{};
                BOOL flushed = FALSE;
                result = get_transform_output(decoding_transform.Get(), decoded_sample.GetAddressOf(), flushed);

                if (result != S_OK && result != MF_E_TRANSFORM_NEED_MORE_INPUT)
                    FAIL("Error getting H264 decoder transform output");

                if (flushed) {
                    // decoder format changed
                } else if (decoded_sample) {
                    // Write decoded sample to capture file.
                }
            }
            count++;
        }
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

TEST_CASE("MFTransform(MP4-YUV) with Coroutine") {
    const auto fpath = fs::absolute(get_asset_dir() / "fm5p7flyCSY.mp4");

    auto on_return = media_startup();

    MF_OBJECT_TYPE ObjectType = MF_OBJECT_INVALID;

    ComPtr<IMFSourceResolver> resolver{};
    ComPtr<IUnknown> source{};
    REQUIRE(MFCreateSourceResolver(resolver.GetAddressOf()) == S_OK);
    REQUIRE(resolver->CreateObjectFromURL(fpath.c_str(),             // URL of the source.
                                          MF_RESOLUTION_MEDIASOURCE, // Create a source object.
                                          NULL,                      // Optional property store.
                                          &ObjectType,               // Receives the created object type.
                                          source.GetAddressOf()      // Receives a pointer to the media source.
                                          ) == S_OK);

    ComPtr<IMFMediaSource> media_source{};
    REQUIRE(source->QueryInterface(IID_PPV_ARGS(media_source.GetAddressOf())) == S_OK);

    ComPtr<IMFAttributes> reader_attributes{};
    REQUIRE(MFCreateAttributes(reader_attributes.GetAddressOf(), 2) == S_OK);
    REQUIRE(reader_attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                       MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID) == S_OK);
    REQUIRE(reader_attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, 1) == S_OK);

    ComPtr<IMFSourceReader> source_reader{};
    REQUIRE(MFCreateSourceReaderFromMediaSource(media_source.Get(), reader_attributes.Get(),
                                                source_reader.GetAddressOf()) == S_OK);

    ComPtr<IMFMediaType> file_video_media_type{};
    REQUIRE(source_reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                               file_video_media_type.GetAddressOf()) == S_OK);
    {
        GUID major{};
        REQUIRE(file_video_media_type->GetMajorType(&major) == S_OK);
        CAPTURE(get_name(major));
        REQUIRE(major == MFMediaType_Video);
    }

    // Create H.264 decoder.
    ComPtr<IUnknown> transform{};
    ComPtr<IMFTransform> decoding_transform{}; // This is H264 Decoder MFT
    REQUIRE(CoCreateInstance(CLSID_CMSH264DecoderMFT, NULL, CLSCTX_INPROC_SERVER, IID_IUnknown,
                             (void**)transform.GetAddressOf()) == S_OK);
    REQUIRE(transform->QueryInterface(IID_PPV_ARGS(decoding_transform.GetAddressOf())) == S_OK);

    ComPtr<IMFMediaType> input_media_type{};
    REQUIRE(MFCreateMediaType(input_media_type.GetAddressOf()) == S_OK);
    REQUIRE(file_video_media_type->CopyAllItems(input_media_type.Get()) == S_OK);
    if (auto hr = decoding_transform->SetInputType(0, input_media_type.Get(), 0))
        FAIL(hr);

    ComPtr<IMFMediaType> output_media_type{};
    REQUIRE(MFCreateMediaType(output_media_type.GetAddressOf()) == S_OK);
    REQUIRE(file_video_media_type->CopyAllItems(output_media_type.Get()) == S_OK);
    if (auto hr = output_media_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_IYUV))
        FAIL(hr);

    if (auto hr = decoding_transform->SetOutputType(0, output_media_type.Get(), 0))
        FAIL(hr);

    DWORD status = 0;
    REQUIRE(decoding_transform->GetInputStatus(0, &status) == S_OK);
    REQUIRE(status == MFT_INPUT_STATUS_ACCEPT_DATA);
    //CAPTURE(GetMediaTypeDescription(output_media_type.Get()));
    REQUIRE(decoding_transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL) == S_OK);
    REQUIRE(decoding_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL) == S_OK);
    REQUIRE(decoding_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL) == S_OK);

    size_t count = 0;
    try {
        for (auto decoded_sample : decode(source_reader, decoding_transform)) {
            if (decoded_sample == nullptr)
                FAIL();
            ++count;
            if (auto hr = check_sample(decoded_sample))
                FAIL(hr);
        }
    } catch (const winrt::hresult_error& ex) {
        // FAIL(ex.message());
        FAIL(ex.code());
    }
    REQUIRE(count > 0);
}
