#include "media.hpp"
#include <dshowasf.h>

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

auto startup() -> gsl::final_action<HRESULT(WINAPI*)()> {
    switch (auto hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {
    case CO_E_NOTINITIALIZED:
        throw _com_error{hr};
    default:
        break;
    }
    if (MFStartup(MF_VERSION) != S_OK)
        throw runtime_error{"MFStartup"};
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
    constexpr UINT32 max_size = 255;
    WCHAR buf[max_size]{};
    UINT32 buflen{};
    HRESULT hr = device->GetString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, buf, max_size, &buflen);
    if (SUCCEEDED(hr))
        name = {buf, buflen};
    return hr;
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
    const char* name = "Unknown";
    if (subtype == MFVideoFormat_RGB32) // 444 (32 bpp)
        name = "D3DFMT_X8R8G8B8";
    else if (subtype == MFVideoFormat_ARGB32)
        name = "D3DFMT_A8R8G8B8";
    else if (subtype == MFVideoFormat_RGB24)
        name = "D3DFMT_R8G8B8";
    else if (subtype == MFVideoFormat_I420) // 420 (16 bpp)
        name = "I420";
    else if (subtype == MFVideoFormat_NV12) // 420 (12 bpp)
        name = "NV12";
    else if (subtype == MFVideoFormat_UYVY) // 422 (12 bpp)
        name = "UYVY";
    else if (subtype == MFVideoFormat_MJPG)
        name = "MJPG";
    else if (subtype == MFVideoFormat_AI44) // 4:4:4 Packed P
        name = "AI44";
    else if (subtype == MFVideoFormat_AYUV) // 4:4:4 Packed 8
        name = "AYUV";
    else if (subtype == MFVideoFormat_I420) // 4:2:0 Planar 8
        name = "I420";
    else if (subtype == MFVideoFormat_IYUV) // 4:2:0 Planar 8
        name = "IYUV";
    else if (subtype == MFVideoFormat_NV11) // 4:1:1 Planar 8
        name = "NV11";
    else if (subtype == MFVideoFormat_NV12) // 4:2:0 Planar 8
        name = "NV12";
    else if (subtype == MFVideoFormat_UYVY) // 4:2:2 Packed 8
        name = "UYVY";
    else if (subtype == MFVideoFormat_Y41P) // 4:1:1 Packed 8
        name = "Y41P";
    else if (subtype == MFVideoFormat_Y41T) // 4:1:1 Packed 8
        name = "Y41T";
    else if (subtype == MFVideoFormat_Y42T) // 4:2:2 Packed 8
        name = "Y42T";
    else if (subtype == MFVideoFormat_YUY2) // 4:2:2 Packed 8
        name = "YUY2";
    else if (subtype == MFVideoFormat_YVU9) // 8:4:4 Planar 9
        name = "YVU9";
    else if (subtype == MFVideoFormat_YV12) // 4:2:0 Planar 8
        name = "YV12";
    else if (subtype == MFVideoFormat_YVYU) // 4:2:2 Packed 8
        name = "YVYU";

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

                if (result != S_OK && result != MF_E_TRANSFORM_NEED_MORE_INPUT)
                    throw winrt::hresult_error{result};

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
