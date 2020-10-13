#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_WINDOWS_CRTDBG
#include <catch2/catch.hpp>

#include <filesystem>
#include <media.hpp>

#include <mferror.h>
#include <mmdeviceapi.h>
#include <wmcodecdsp.h>
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
    read_on_background(impl.reader);
}

HRESULT write_sample(IMFSample* pSample, std::ofstream& stream) {
    ComPtr<IMFMediaBuffer> buffer{};
    if (auto hr = pSample->ConvertToContiguousBuffer(buffer.GetAddressOf()))
        return hr;
    DWORD bufsz{};
    if (auto hr = buffer->GetCurrentLength(&bufsz))
        return hr;

    BYTE* ptr = NULL;
    DWORD capacity = 0, length = 0;
    if (auto hr = buffer->Lock(&ptr, &capacity, &length))
        return hr;
    auto on_return = gsl::finally([buffer]() { buffer->Unlock(); });

    stream.write((char*)ptr, bufsz);
    stream.flush();
    return S_OK;
}

HRESULT create_single_buffer_sample(DWORD bufsz, IMFSample** sample) {
    if (auto hr = MFCreateSample(sample))
        return hr;
    ComPtr<IMFMediaBuffer> buffer{};
    if (auto hr = MFCreateMemoryBuffer(bufsz, buffer.GetAddressOf()))
        return hr;
    return (*sample)->AddBuffer(buffer.Get());
}

HRESULT create_and_copy_single_buffer_sample(IMFSample* src, IMFSample** dst) {
    DWORD total{};
    if (auto hr = src->GetTotalLength(&total))
        return hr;
    if (auto hr = create_single_buffer_sample(total, dst))
        return hr;
    if (auto hr = src->CopyAllItems(*dst))
        return hr;
    ComPtr<IMFMediaBuffer> buffer{};
    if (auto hr = (*dst)->GetBufferByIndex(0, buffer.GetAddressOf()))
        return hr;
    return src->CopyToBuffer(buffer.Get());
}

HRESULT get_transform_output(IMFTransform* transform, IMFSample** sample, BOOL& flushed) {
    MFT_OUTPUT_STREAM_INFO stream_info{};
    if (auto hr = transform->GetOutputStreamInfo(0, &stream_info))
        return hr;

    flushed = FALSE;
    *sample = nullptr;

    MFT_OUTPUT_DATA_BUFFER output{};
    if ((stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
        if (auto hr = create_single_buffer_sample(stream_info.cbSize, sample))
            return hr;
        output.pSample = *sample;
    }

    DWORD status = 0;
    HRESULT const result = transform->ProcessOutput(0, 1, &output, &status);
    if (result == S_OK) {
        *sample = output.pSample;
        return S_OK;
    }

    // see https://docs.microsoft.com/en-us/windows/win32/medfound/handling-stream-changes
    if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
        ComPtr<IMFMediaType> changed_output_type{};
        if (output.dwStatus != MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE) {
            // todo: add more works for this case
            return E_NOTIMPL;
        }

        if (auto hr = transform->GetOutputAvailableType(0, 0, changed_output_type.GetAddressOf()))
            return hr;

        // check new output media type

        if (auto hr = changed_output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_IYUV))
            return hr;
        if (auto hr = transform->SetOutputType(0, changed_output_type.Get(), 0))
            return hr;

        if (auto hr = transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL))
            return hr;
        flushed = TRUE;

        return S_OK;
    }
    // MF_E_TRANSFORM_NEED_MORE_INPUT: not an error condition but it means the allocated output sample is empty.
    return result;
}

gsl::czstring<> get_name(const GUID& guid) noexcept;

TEST_CASE("MFTransform(MP4-YUV)") {
    const auto fpath = fs::absolute(get_asset_dir() / "fm5p7flyCSY.mp4");
    std::ofstream outputBuffer("output.mp4", std::ios::out | std::ios::binary);

    auto on_return = startup();

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

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/media-type-debugging-code
gsl::czstring<> get_name(const GUID& guid) noexcept {
#ifndef IF_EQUAL_RETURN
#define IF_EQUAL_RETURN(param, val)                                                                                    \
    if (val == param)                                                                                                  \
    return #val
#endif
    IF_EQUAL_RETURN(guid, MF_MT_MAJOR_TYPE);
    IF_EQUAL_RETURN(guid, MF_MT_MAJOR_TYPE);
    IF_EQUAL_RETURN(guid, MF_MT_SUBTYPE);
    IF_EQUAL_RETURN(guid, MF_MT_ALL_SAMPLES_INDEPENDENT);
    IF_EQUAL_RETURN(guid, MF_MT_FIXED_SIZE_SAMPLES);
    IF_EQUAL_RETURN(guid, MF_MT_COMPRESSED);
    IF_EQUAL_RETURN(guid, MF_MT_SAMPLE_SIZE);
    IF_EQUAL_RETURN(guid, MF_MT_WRAPPED_TYPE);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_NUM_CHANNELS);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_SAMPLES_PER_SECOND);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_FLOAT_SAMPLES_PER_SECOND);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_AVG_BYTES_PER_SECOND);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_BLOCK_ALIGNMENT);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_BITS_PER_SAMPLE);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_VALID_BITS_PER_SAMPLE);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_SAMPLES_PER_BLOCK);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_CHANNEL_MASK);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_FOLDDOWN_MATRIX);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_WMADRC_PEAKREF);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_WMADRC_PEAKTARGET);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_WMADRC_AVGREF);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_WMADRC_AVGTARGET);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_PREFER_WAVEFORMATEX);
    IF_EQUAL_RETURN(guid, MF_MT_AAC_PAYLOAD_TYPE);
    IF_EQUAL_RETURN(guid, MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION);
    IF_EQUAL_RETURN(guid, MF_MT_FRAME_SIZE);
    IF_EQUAL_RETURN(guid, MF_MT_FRAME_RATE);
    IF_EQUAL_RETURN(guid, MF_MT_FRAME_RATE_RANGE_MAX);
    IF_EQUAL_RETURN(guid, MF_MT_FRAME_RATE_RANGE_MIN);
    IF_EQUAL_RETURN(guid, MF_MT_PIXEL_ASPECT_RATIO);
    IF_EQUAL_RETURN(guid, MF_MT_DRM_FLAGS);
    IF_EQUAL_RETURN(guid, MF_MT_PAD_CONTROL_FLAGS);
    IF_EQUAL_RETURN(guid, MF_MT_SOURCE_CONTENT_HINT);
    IF_EQUAL_RETURN(guid, MF_MT_VIDEO_CHROMA_SITING);
    IF_EQUAL_RETURN(guid, MF_MT_INTERLACE_MODE);
    IF_EQUAL_RETURN(guid, MF_MT_TRANSFER_FUNCTION);
    IF_EQUAL_RETURN(guid, MF_MT_VIDEO_PRIMARIES);
    IF_EQUAL_RETURN(guid, MF_MT_CUSTOM_VIDEO_PRIMARIES);
    IF_EQUAL_RETURN(guid, MF_MT_YUV_MATRIX);
    IF_EQUAL_RETURN(guid, MF_MT_VIDEO_LIGHTING);
    IF_EQUAL_RETURN(guid, MF_MT_VIDEO_NOMINAL_RANGE);
    IF_EQUAL_RETURN(guid, MF_MT_GEOMETRIC_APERTURE);
    IF_EQUAL_RETURN(guid, MF_MT_MINIMUM_DISPLAY_APERTURE);
    IF_EQUAL_RETURN(guid, MF_MT_PAN_SCAN_APERTURE);
    IF_EQUAL_RETURN(guid, MF_MT_PAN_SCAN_ENABLED);
    IF_EQUAL_RETURN(guid, MF_MT_AVG_BITRATE);
    IF_EQUAL_RETURN(guid, MF_MT_AVG_BIT_ERROR_RATE);
    IF_EQUAL_RETURN(guid, MF_MT_MAX_KEYFRAME_SPACING);
    IF_EQUAL_RETURN(guid, MF_MT_DEFAULT_STRIDE);
    IF_EQUAL_RETURN(guid, MF_MT_PALETTE);
    IF_EQUAL_RETURN(guid, MF_MT_USER_DATA);
    IF_EQUAL_RETURN(guid, MF_MT_AM_FORMAT_TYPE);
    IF_EQUAL_RETURN(guid, MF_MT_MPEG_START_TIME_CODE);
    IF_EQUAL_RETURN(guid, MF_MT_MPEG2_PROFILE);
    IF_EQUAL_RETURN(guid, MF_MT_MPEG2_LEVEL);
    IF_EQUAL_RETURN(guid, MF_MT_MPEG2_FLAGS);
    IF_EQUAL_RETURN(guid, MF_MT_MPEG_SEQUENCE_HEADER);
    IF_EQUAL_RETURN(guid, MF_MT_DV_AAUX_SRC_PACK_0);
    IF_EQUAL_RETURN(guid, MF_MT_DV_AAUX_CTRL_PACK_0);
    IF_EQUAL_RETURN(guid, MF_MT_DV_AAUX_SRC_PACK_1);
    IF_EQUAL_RETURN(guid, MF_MT_DV_AAUX_CTRL_PACK_1);
    IF_EQUAL_RETURN(guid, MF_MT_DV_VAUX_SRC_PACK);
    IF_EQUAL_RETURN(guid, MF_MT_DV_VAUX_CTRL_PACK);
    IF_EQUAL_RETURN(guid, MF_MT_ARBITRARY_HEADER);
    IF_EQUAL_RETURN(guid, MF_MT_ARBITRARY_FORMAT);
    IF_EQUAL_RETURN(guid, MF_MT_IMAGE_LOSS_TOLERANT);
    IF_EQUAL_RETURN(guid, MF_MT_MPEG4_SAMPLE_DESCRIPTION);
    IF_EQUAL_RETURN(guid, MF_MT_MPEG4_CURRENT_SAMPLE_ENTRY);
    IF_EQUAL_RETURN(guid, MF_MT_ORIGINAL_4CC);
    IF_EQUAL_RETURN(guid, MF_MT_ORIGINAL_WAVE_FORMAT_TAG);

    // Media types

    IF_EQUAL_RETURN(guid, MFMediaType_Audio);
    IF_EQUAL_RETURN(guid, MFMediaType_Video);
    IF_EQUAL_RETURN(guid, MFMediaType_Protected);
    IF_EQUAL_RETURN(guid, MFMediaType_SAMI);
    IF_EQUAL_RETURN(guid, MFMediaType_Script);
    IF_EQUAL_RETURN(guid, MFMediaType_Image);
    IF_EQUAL_RETURN(guid, MFMediaType_HTML);
    IF_EQUAL_RETURN(guid, MFMediaType_Binary);
    IF_EQUAL_RETURN(guid, MFMediaType_FileTransfer);

    IF_EQUAL_RETURN(guid, MFVideoFormat_AI44);   //     FCC('AI44')
    IF_EQUAL_RETURN(guid, MFVideoFormat_ARGB32); //   D3DFMT_A8R8G8B8
    IF_EQUAL_RETURN(guid, MFVideoFormat_AYUV);   //     FCC('AYUV')
    IF_EQUAL_RETURN(guid, MFVideoFormat_DV25);   //     FCC('dv25')
    IF_EQUAL_RETURN(guid, MFVideoFormat_DV50);   //     FCC('dv50')
    IF_EQUAL_RETURN(guid, MFVideoFormat_DVH1);   //     FCC('dvh1')
    IF_EQUAL_RETURN(guid, MFVideoFormat_DVSD);   //     FCC('dvsd')
    IF_EQUAL_RETURN(guid, MFVideoFormat_DVSL);   //     FCC('dvsl')
    IF_EQUAL_RETURN(guid, MFVideoFormat_H264);   //     FCC('H264')
    IF_EQUAL_RETURN(guid, MFVideoFormat_I420);   //     FCC('I420')
    IF_EQUAL_RETURN(guid, MFVideoFormat_IYUV);   //     FCC('IYUV')
    IF_EQUAL_RETURN(guid, MFVideoFormat_M4S2);   //     FCC('M4S2')
    IF_EQUAL_RETURN(guid, MFVideoFormat_MJPG);
    IF_EQUAL_RETURN(guid, MFVideoFormat_MP43);   //     FCC('MP43')
    IF_EQUAL_RETURN(guid, MFVideoFormat_MP4S);   //     FCC('MP4S')
    IF_EQUAL_RETURN(guid, MFVideoFormat_MP4V);   //     FCC('MP4V')
    IF_EQUAL_RETURN(guid, MFVideoFormat_MPG1);   //     FCC('MPG1')
    IF_EQUAL_RETURN(guid, MFVideoFormat_MSS1);   //     FCC('MSS1')
    IF_EQUAL_RETURN(guid, MFVideoFormat_MSS2);   //     FCC('MSS2')
    IF_EQUAL_RETURN(guid, MFVideoFormat_NV11);   //     FCC('NV11')
    IF_EQUAL_RETURN(guid, MFVideoFormat_NV12);   //     FCC('NV12')
    IF_EQUAL_RETURN(guid, MFVideoFormat_P010);   //     FCC('P010')
    IF_EQUAL_RETURN(guid, MFVideoFormat_P016);   //     FCC('P016')
    IF_EQUAL_RETURN(guid, MFVideoFormat_P210);   //     FCC('P210')
    IF_EQUAL_RETURN(guid, MFVideoFormat_P216);   //     FCC('P216')
    IF_EQUAL_RETURN(guid, MFVideoFormat_RGB24);  //    D3DFMT_R8G8B8
    IF_EQUAL_RETURN(guid, MFVideoFormat_RGB32);  //    D3DFMT_X8R8G8B8
    IF_EQUAL_RETURN(guid, MFVideoFormat_RGB555); //   D3DFMT_X1R5G5B5
    IF_EQUAL_RETURN(guid, MFVideoFormat_RGB565); //   D3DFMT_R5G6B5
    IF_EQUAL_RETURN(guid, MFVideoFormat_RGB8);
    IF_EQUAL_RETURN(guid, MFVideoFormat_UYVY); //     FCC('UYVY')
    IF_EQUAL_RETURN(guid, MFVideoFormat_v210); //     FCC('v210')
    IF_EQUAL_RETURN(guid, MFVideoFormat_v410); //     FCC('v410')
    IF_EQUAL_RETURN(guid, MFVideoFormat_WMV1); //     FCC('WMV1')
    IF_EQUAL_RETURN(guid, MFVideoFormat_WMV2); //     FCC('WMV2')
    IF_EQUAL_RETURN(guid, MFVideoFormat_WMV3); //     FCC('WMV3')
    IF_EQUAL_RETURN(guid, MFVideoFormat_WVC1); //     FCC('WVC1')
    IF_EQUAL_RETURN(guid, MFVideoFormat_Y210); //     FCC('Y210')
    IF_EQUAL_RETURN(guid, MFVideoFormat_Y216); //     FCC('Y216')
    IF_EQUAL_RETURN(guid, MFVideoFormat_Y410); //     FCC('Y410')
    IF_EQUAL_RETURN(guid, MFVideoFormat_Y416); //     FCC('Y416')
    IF_EQUAL_RETURN(guid, MFVideoFormat_Y41P);
    IF_EQUAL_RETURN(guid, MFVideoFormat_Y41T);
    IF_EQUAL_RETURN(guid, MFVideoFormat_YUY2); //     FCC('YUY2')
    IF_EQUAL_RETURN(guid, MFVideoFormat_YV12); //     FCC('YV12')
    IF_EQUAL_RETURN(guid, MFVideoFormat_YVYU);

    IF_EQUAL_RETURN(guid, MFAudioFormat_PCM);              //              WAVE_FORMAT_PCM
    IF_EQUAL_RETURN(guid, MFAudioFormat_Float);            //            WAVE_FORMAT_IEEE_FLOAT
    IF_EQUAL_RETURN(guid, MFAudioFormat_DTS);              //              WAVE_FORMAT_DTS
    IF_EQUAL_RETURN(guid, MFAudioFormat_Dolby_AC3_SPDIF);  //  WAVE_FORMAT_DOLBY_AC3_SPDIF
    IF_EQUAL_RETURN(guid, MFAudioFormat_DRM);              //              WAVE_FORMAT_DRM
    IF_EQUAL_RETURN(guid, MFAudioFormat_WMAudioV8);        //        WAVE_FORMAT_WMAUDIO2
    IF_EQUAL_RETURN(guid, MFAudioFormat_WMAudioV9);        //        WAVE_FORMAT_WMAUDIO3
    IF_EQUAL_RETURN(guid, MFAudioFormat_WMAudio_Lossless); // WAVE_FORMAT_WMAUDIO_LOSSLESS
    IF_EQUAL_RETURN(guid, MFAudioFormat_WMASPDIF);         //         WAVE_FORMAT_WMASPDIF
    IF_EQUAL_RETURN(guid, MFAudioFormat_MSP1);             //             WAVE_FORMAT_WMAVOICE9
    IF_EQUAL_RETURN(guid, MFAudioFormat_MP3);              //              WAVE_FORMAT_MPEGLAYER3
    IF_EQUAL_RETURN(guid, MFAudioFormat_MPEG);             //             WAVE_FORMAT_MPEG
    IF_EQUAL_RETURN(guid, MFAudioFormat_AAC);              //              WAVE_FORMAT_MPEG_HEAAC
    IF_EQUAL_RETURN(guid, MFAudioFormat_ADTS);             //             WAVE_FORMAT_MPEG_ADTS_AAC
#undef IF_EQUAL_RETURN
    return NULL;
}

/**
* Helper function to get a user friendly description for a media type.
* Note that there may be properties missing or incorrectly described.
* @param[in] pMediaType: pointer to the media type to get a description for.
* @@Returns A string describing the media type.
*
* Potential improvements https://docs.microsoft.com/en-us/windows/win32/medfound/media-type-debugging-code.
*/
std::string GetMediaTypeDescription(IMFMediaType* pMediaType) {
    HRESULT hr = S_OK;
    GUID MajorType;
    UINT32 cAttrCount;
    LPCSTR pszGuidStr;
    std::string description;
    WCHAR TempBuf[200];

    if (pMediaType == NULL) {
        description = "<NULL>";
        goto done;
    }

    hr = pMediaType->GetMajorType(&MajorType);
    if FAILED (hr)
        goto done;

    //pszGuidStr = STRING_FROM_GUID(MajorType);
    pszGuidStr = get_name(MajorType);
    if (pszGuidStr != NULL) {
        description += pszGuidStr;
        description += ": ";
    } else {
        description += "Other: ";
    }

    hr = pMediaType->GetCount(&cAttrCount);
    if FAILED (hr)
        goto done;

    for (UINT32 i = 0; i < cAttrCount; i++) {
        GUID guidId;
        MF_ATTRIBUTE_TYPE attrType;

        hr = pMediaType->GetItemByIndex(i, &guidId, NULL);
        if FAILED (hr)
            goto done;

        hr = pMediaType->GetItemType(guidId, &attrType);
        if FAILED (hr)
            goto done;

        //pszGuidStr = STRING_FROM_GUID(guidId);
        pszGuidStr = get_name(guidId);
        if (pszGuidStr != NULL) {
            description += pszGuidStr;
        } else {
            LPOLESTR guidStr = NULL;

            hr = StringFromCLSID(guidId, &guidStr);
            if FAILED (hr)
                goto done;

            auto wGuidStr = std::wstring(guidStr);
            description += std::string(wGuidStr.begin(), wGuidStr.end()); // GUID's won't have wide chars.

            CoTaskMemFree(guidStr);
        }

        description += "=";

        switch (attrType) {
        case MF_ATTRIBUTE_UINT32: {
            UINT32 Val;
            hr = pMediaType->GetUINT32(guidId, &Val);
            if FAILED (hr)
                goto done;

            description += std::to_string(Val);
            break;
        }
        case MF_ATTRIBUTE_UINT64: {
            UINT64 Val;
            hr = pMediaType->GetUINT64(guidId, &Val);
            if FAILED (hr)
                goto done;

            if (guidId == MF_MT_FRAME_SIZE) {
                description += "W:" + std::to_string(HI32(Val)) + " H: " + std::to_string(LO32(Val));
            } else if (guidId == MF_MT_FRAME_RATE) {
                // Frame rate is numerator/denominator.
                description += std::to_string(HI32(Val)) + "/" + std::to_string(LO32(Val));
            } else if (guidId == MF_MT_PIXEL_ASPECT_RATIO) {
                description += std::to_string(HI32(Val)) + ":" + std::to_string(LO32(Val));
            } else {
                //tempStr.Format("%ld", Val);
                description += std::to_string(Val);
            }

            //description += tempStr;

            break;
        }
        case MF_ATTRIBUTE_DOUBLE: {
            DOUBLE Val;
            hr = pMediaType->GetDouble(guidId, &Val);
            if FAILED (hr)
                goto done;

            //tempStr.Format("%f", Val);
            description += std::to_string(Val);
            break;
        }
        case MF_ATTRIBUTE_GUID: {
            GUID Val;
            const char* pValStr;

            hr = pMediaType->GetGUID(guidId, &Val);
            if FAILED (hr)
                goto done;

            //pValStr = STRING_FROM_GUID(Val);
            pValStr = get_name(Val);
            if (pValStr != NULL) {
                description += pValStr;
            } else {
                LPOLESTR guidStr = NULL;
                hr = StringFromCLSID(Val, &guidStr);
                if FAILED (hr)
                    goto done;

                auto wGuidStr = std::wstring(guidStr);
                description += std::string(wGuidStr.begin(), wGuidStr.end()); // GUID's won't have wide chars.

                CoTaskMemFree(guidStr);
            }

            break;
        }
        case MF_ATTRIBUTE_STRING: {
            hr = pMediaType->GetString(guidId, TempBuf, sizeof(TempBuf) / sizeof(TempBuf[0]), NULL);
            if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) {
                description += "<Too Long>";
                break;
            }
            if FAILED (hr)
                goto done;

            auto wstr = std::wstring(TempBuf);
            description += std::string(
                wstr.begin(), wstr.end()); // It's unlikely the attribute descriptions will contain multi byte chars.

            break;
        }
        case MF_ATTRIBUTE_BLOB: {
            description += "<BLOB>";
            break;
        }
        case MF_ATTRIBUTE_IUNKNOWN: {
            description += "<UNK>";
            break;
        }
        }

        description += ", ";
    }

done:

    return description;
}
