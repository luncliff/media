#include "media.hpp"

#include <dshowasf.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/wincolor_sink.h>
#include <spdlog/spdlog.h>

using namespace std;
using namespace Microsoft::WRL;

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

auto media_startup() noexcept(false) -> gsl::final_action<HRESULT(WINAPI*)()> {
    if (auto hr = MFStartup(MF_VERSION))
        throw winrt::hresult_error{hr};
    return gsl::finally(&MFShutdown);
}

HRESULT get_devices(IMFAttributes* attrs, vector<ComPtr<IMFActivate>>& devices) noexcept {
    IMFActivate** handles = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFEnumDeviceSources(attrs, &handles, &count);
    if FAILED (hr)
        return hr;
    auto on_return = gsl::finally([handles]() { CoTaskMemFree(handles); });
    for (auto i = 0u; i < count; ++i)
        devices.emplace_back(move(handles[i]));
    return S_OK;
}

HRESULT get_name(IMFActivate* device, wstring& name) noexcept {
    constexpr UINT32 max_size = 240;
    WCHAR buf[max_size]{};
    UINT32 buflen{};
    HRESULT hr = device->GetString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, buf, max_size, &buflen);
    if (SUCCEEDED(hr))
        name = {buf, buflen};
    return hr;
}

HRESULT resolve(const fs::path& fpath, IMFMediaSourceEx** source, MF_OBJECT_TYPE& media_object_type) noexcept {
    ComPtr<IMFSourceResolver> resolver{};
    if (auto hr = MFCreateSourceResolver(resolver.GetAddressOf()))
        return hr;
    ComPtr<IUnknown> unknown{};
    if (auto hr = resolver->CreateObjectFromURL(fpath.c_str(), MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_READ, NULL,
                                                &media_object_type, unknown.GetAddressOf()))
        return hr;
    return unknown->QueryInterface(IID_PPV_ARGS(source));
}

HRESULT make_transform_H264(IMFTransform** transform) noexcept {
    ComPtr<IUnknown> unknown{};
    if (auto hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, NULL, CLSCTX_INPROC_SERVER, IID_IUnknown,
                                   (void**)unknown.GetAddressOf()))
        return hr;
    return unknown->QueryInterface(IID_PPV_ARGS(transform));
}

HRESULT make_transform_video(IMFTransform** transform) noexcept {
    ComPtr<IUnknown> unknown{};
    if (auto hr = CoCreateInstance(CLSID_VideoProcessorMFT, NULL, CLSCTX_INPROC_SERVER, IID_IUnknown,
                                   (void**)unknown.GetAddressOf()))
        return hr;
    return unknown->QueryInterface(IID_PPV_ARGS(transform));
}

HRESULT get_stream_descriptor(IMFPresentationDescriptor* presentation, IMFStreamDescriptor** ptr) {
    DWORD num_stream = 0;
    if (auto hr = presentation->GetStreamDescriptorCount(&num_stream); SUCCEEDED(hr) == false)
        return hr;
    for (auto i = 0u; i < num_stream; ++i) {
        BOOL selected = false;
        if (auto hr = presentation->GetStreamDescriptorByIndex(i, &selected, ptr); FAILED(hr))
            return hr;
        if (selected)
            break;
    }
    return S_OK;
}

HRESULT configure_video(ComPtr<IMFMediaType> type) {
    GUID subtype{};
    type->GetGUID(MF_MT_SUBTYPE, &subtype);

    UINT32 interlace = 0;
    type->GetUINT32(MF_MT_INTERLACE_MODE, &interlace);
    const auto imode = static_cast<MFVideoInterlaceMode>(interlace);

    UINT32 stride = 0;
    type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride);

    UINT32 ycbcr2rgb = 0;
    type->GetUINT32(MF_MT_YUV_MATRIX, &ycbcr2rgb);
    const auto matrix = static_cast<MFVideoTransferMatrix>(ycbcr2rgb);

    UINT64 size = 0;
    type->GetUINT64(MF_MT_FRAME_SIZE, &size); // MFGetAttributeSize
    UINT32 w = size >> 32, h = size & UINT32_MAX;

    UINT64 framerate = 0; // framerate >> 32;
    type->GetUINT64(MF_MT_FRAME_RATE_RANGE_MAX, &framerate);
    return type->SetUINT64(MF_MT_FRAME_RATE, framerate);
}

void print(IMFMediaType* media) noexcept;

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/video-processor-mft
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/video-media-types
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/about-yuv-video
HRESULT configure(ComPtr<IMFStreamDescriptor> stream) noexcept {
    ComPtr<IMFMediaTypeHandler> handler{};
    if (auto hr = stream->GetMediaTypeHandler(handler.GetAddressOf()); SUCCEEDED(hr) == false)
        return hr;
    DWORD num_types = 0;
    if (auto hr = handler->GetMediaTypeCount(&num_types); SUCCEEDED(hr) == false)
        return hr;
    ComPtr<IMFMediaType> type{};
    for (auto i = 0u; i < num_types; ++i) {
        ComPtr<IMFMediaType> current{};
        if (auto hr = handler->GetMediaTypeByIndex(i, current.GetAddressOf()); FAILED(hr))
            return hr;
        if (type == nullptr)
            type = current;

        print(current.Get());
    }
    return handler->SetCurrentMediaType(type.Get());
}

/// @see https://docs.microsoft.com/en-us/windows/win32/wmformat/media-type-identifiers
void print_video(FILE* fout, IMFMediaType* media) noexcept {
    fprintf(fout, " - video:\n");
    GUID subtype{};
    media->GetGUID(MF_MT_SUBTYPE, &subtype);
    gsl::czstring<> name = get_name(subtype);
    if (name == nullptr)
        name = "Unknown";
    // MFVideoFormat_RGB32 // 444 (32 bpp)
    // MFVideoFormat_ARGB32
    // MFVideoFormat_RGB24
    // MFVideoFormat_I420 // 420 (16 bpp)
    // MFVideoFormat_NV12 // 420 (12 bpp)
    // MFVideoFormat_UYVY // 422 (12 bpp)
    // MFVideoFormat_MJPG
    // MFVideoFormat_AI44 // 4:4:4 Packed P
    // MFVideoFormat_AYUV // 4:4:4 Packed 8
    // MFVideoFormat_I420 // 4:2:0 Planar 8
    // MFVideoFormat_IYUV // 4:2:0 Planar 8
    // MFVideoFormat_NV11 // 4:1:1 Planar 8
    // MFVideoFormat_NV12 // 4:2:0 Planar 8
    // MFVideoFormat_UYVY // 4:2:2 Packed 8
    // MFVideoFormat_Y41P // 4:1:1 Packed 8
    // MFVideoFormat_Y41T // 4:1:1 Packed 8
    // MFVideoFormat_Y42T // 4:2:2 Packed 8
    // MFVideoFormat_YUY2 // 4:2:2 Packed 8
    // MFVideoFormat_YVU9 // 8:4:4 Planar 9
    // MFVideoFormat_YV12 // 4:2:0 Planar 8
    // MFVideoFormat_YVYU // 4:2:2 Packed 8
    fprintf(fout, "   - format: %s\n", name);
    UINT64 value = 0;
    if (auto hr = media->GetUINT64(MF_MT_FRAME_SIZE, &value); SUCCEEDED(hr)) {
        UINT32 w = value >> 32, h = value & UINT32_MAX;
        fprintf(fout, "   - width: %u\n", w);
        fprintf(fout, "   - height: %u\n", h);
    }
    if (auto hr = media->GetUINT64(MF_MT_FRAME_RATE_RANGE_MAX, &value); SUCCEEDED(hr)) {
        UINT32 framerate = value >> 32;
        fprintf(fout, "   - fps: %u\n", framerate);
    }
}

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/video-subtype-guids
/// @see https://stackoverflow.com/a/9681384
void print(IMFMediaType* media) noexcept {
    GUID major{};
    media->GetGUID(MF_MT_MAJOR_TYPE, &major);
    if (major == MFMediaType_Default)
        fprintf(stdout, " - default\n");
    else if (major == MFMediaType_Audio)
        fprintf(stdout, " - audio\n");
    else if (major == MFMediaType_Stream)
        fprintf(stdout, " - stream\n");
    else if (major == MFMediaType_Video)
        print_video(stdout, media);
    else
        fprintf(stdout, " - unknown\n");
}

auto decode(ComPtr<IMFSourceReader> source_reader, ComPtr<IMFTransform> decoding_transform)
    -> std::experimental::generator<ComPtr<IMFSample>> {
    while (true) {
        ComPtr<IMFSample> video_sample{};
        DWORD stream_index{};
        DWORD flags{};
        LONGLONG sample_timestamp = 0; // unit 100-nanosecond
        LONGLONG sample_duration = 0;
        if (auto hr = source_reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream_index, &flags,
                                                &sample_timestamp, video_sample.GetAddressOf()))
            throw winrt::hresult_error{hr};

        if (flags & MF_SOURCE_READERF_STREAMTICK) {
            // INFO("MF_SOURCE_READERF_STREAMTICK");
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            // INFO("MF_SOURCE_READERF_ENDOFSTREAM");
            break;
        }
        if (flags & MF_SOURCE_READERF_NEWSTREAM) {
            // INFO("MF_SOURCE_READERF_NEWSTREAM");
            break;
        }
        if (flags & MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED) {
            // INFO("MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED");
            break;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            // INFO("MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED");
            break;
        }

        if (video_sample) {
            if (auto hr = video_sample->SetSampleTime(sample_timestamp))
                throw winrt::hresult_error{hr};
            if (auto hr = video_sample->GetSampleDuration(&sample_duration))
                throw winrt::hresult_error{hr};
            DWORD flags = 0;
            if (auto hr = video_sample->GetSampleFlags(&flags))
                throw winrt::hresult_error{hr};

            // Replicate transmitting the sample across the network and reconstructing.
            ComPtr<IMFSample> copied_sample{};
            if (auto hr = create_and_copy_single_buffer_sample(video_sample.Get(), copied_sample.GetAddressOf()))
                throw winrt::hresult_error{hr};

            // Apply the H264 decoder transform
            if (auto hr = decoding_transform->ProcessInput(0, copied_sample.Get(), 0))
                throw winrt::hresult_error{hr};

            HRESULT result = S_OK;
            while (result == S_OK) {
                ComPtr<IMFSample> decoded_sample{};
                BOOL flushed = FALSE;
                result = get_transform_output(decoding_transform.Get(), decoded_sample.GetAddressOf(), flushed);

                switch (result) {
                case S_OK:
                case MF_E_TRANSFORM_NEED_MORE_INPUT:
                    break;
                case MF_E_INVALIDMEDIATYPE:
                default:
                    throw winrt::hresult_error{result};
                }

                if (flushed) {
                    // decoder format changed
                } else if (decoded_sample) {
                    // Write decoded sample to capture file.
                    co_yield decoded_sample;
                }
            }
        }
    }
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

    IF_EQUAL_RETURN(guid, MFVideoFormat_AI44);    //     FCC('AI44')
    IF_EQUAL_RETURN(guid, MFVideoFormat_ARGB32);  //   D3DFMT_A8R8G8B8
    IF_EQUAL_RETURN(guid, MFVideoFormat_AYUV);    //     FCC('AYUV')
    IF_EQUAL_RETURN(guid, MFVideoFormat_DV25);    //     FCC('dv25')
    IF_EQUAL_RETURN(guid, MFVideoFormat_DV50);    //     FCC('dv50')
    IF_EQUAL_RETURN(guid, MFVideoFormat_DVH1);    //     FCC('dvh1')
    IF_EQUAL_RETURN(guid, MFVideoFormat_DVSD);    //     FCC('dvsd')
    IF_EQUAL_RETURN(guid, MFVideoFormat_DVSL);    //     FCC('dvsl')
    IF_EQUAL_RETURN(guid, MFVideoFormat_H264);    //     FCC('H264')
    IF_EQUAL_RETURN(guid, MFVideoFormat_H264_ES); //
    IF_EQUAL_RETURN(guid, MFVideoFormat_I420);    //     FCC('I420')
    IF_EQUAL_RETURN(guid, MFVideoFormat_IYUV);    //     FCC('IYUV')
    IF_EQUAL_RETURN(guid, MFVideoFormat_M4S2);    //     FCC('M4S2')
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
