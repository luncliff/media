#include "media.hpp"

#include <dshowasf.h>
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

HRESULT get_devices(gsl::not_null<IMFAttributes*> attrs, vector<ComPtr<IMFActivate>>& devices) noexcept {
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

std::string w2mb(std::wstring_view in) noexcept(false);

HRESULT get_name(gsl::not_null<IMFActivate*> device, std::string& name) noexcept {
    wstring wtxt{};
    auto hr = get_name(device, wtxt);
    if SUCCEEDED (hr)
        name = w2mb(wtxt);
    return hr;
}

HRESULT get_name(gsl::not_null<IMFActivate*> device, std::wstring& name) noexcept {
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

auto get_input_available_types(ComPtr<IMFTransform> transform, DWORD num_input, HRESULT& ec) noexcept(false)
    -> std::experimental::generator<ComPtr<IMFMediaType>> {
    for (auto stream_id = 0u; stream_id < num_input; ++stream_id) {
        DWORD type_index = 0;
        ComPtr<IMFMediaType> type{};
        for (ec = transform->GetInputAvailableType(stream_id, type_index++, type.GetAddressOf()); SUCCEEDED(ec);
             ec = transform->GetInputAvailableType(stream_id, type_index++, type.ReleaseAndGetAddressOf())) {
            co_yield type;
        }
    }
}
auto try_output_available_types(ComPtr<IMFTransform> transform, DWORD stream_id, DWORD& type_index) noexcept(false)
    -> std::experimental::generator<ComPtr<IMFMediaType>> {
    type_index = 0;
    ComPtr<IMFMediaType> media_type{};
    for (auto hr = transform->GetOutputAvailableType(stream_id, type_index++, media_type.GetAddressOf()); SUCCEEDED(hr);
         hr = transform->GetOutputAvailableType(stream_id, type_index++, media_type.ReleaseAndGetAddressOf()))
        co_yield media_type;
}

auto get_output_available_types(ComPtr<IMFTransform> transform, DWORD num_output, HRESULT& ec) noexcept(false)
    -> std::experimental::generator<ComPtr<IMFMediaType>> {
    for (auto stream_id = 0u; stream_id < num_output; ++stream_id) {
        DWORD type_index = 0;
        ComPtr<IMFMediaType> type{};
        for (ec = transform->GetOutputAvailableType(stream_id, type_index++, type.GetAddressOf()); SUCCEEDED(ec);
             ec = transform->GetOutputAvailableType(stream_id, type_index++, type.ReleaseAndGetAddressOf())) {
            co_yield type;
        }
    }
}
auto try_input_available_types(ComPtr<IMFTransform> transform, DWORD stream_id, DWORD& type_index) noexcept(false)
    -> std::experimental::generator<ComPtr<IMFMediaType>> {
    type_index = 0;
    ComPtr<IMFMediaType> media_type{};
    for (auto hr = transform->GetInputAvailableType(stream_id, type_index++, media_type.GetAddressOf()); SUCCEEDED(hr);
         hr = transform->GetInputAvailableType(stream_id, type_index++, media_type.ReleaseAndGetAddressOf()))
        co_yield media_type;
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

auto read_samples(ComPtr<IMFSourceReader> source_reader, //
                  DWORD& index, DWORD& flags, LONGLONG& timestamp, LONGLONG& duration) noexcept(false)
    -> std::experimental::generator<ComPtr<IMFSample>> {
    while (true) {
        ComPtr<IMFSample> input_sample{};
        if (auto hr = source_reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &index, &flags, &timestamp,
                                                input_sample.GetAddressOf()))
            throw winrt::hresult_error{hr};

        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            co_return;
        if (input_sample == nullptr) // probably MF_SOURCE_READERF_STREAMTICK
            continue;
        co_yield input_sample;
    }
};

auto decode(ComPtr<IMFTransform> transform, ComPtr<IMFMediaType> output_type, //
            DWORD ostream_id) noexcept(false) -> std::experimental::generator<ComPtr<IMFSample>> {
    MFT_OUTPUT_STREAM_INFO output_stream_info{};
    if (auto hr = transform->GetOutputStreamInfo(ostream_id, &output_stream_info))
        throw winrt::hresult_error{hr};

    while (true) {
        MFT_OUTPUT_DATA_BUFFER output_buffer{};
        ComPtr<IMFSample> output_sample{};
        if (output_stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) {
            // ...
        } else {
            if (auto hr = create_single_buffer_sample(output_stream_info.cbSize, output_sample.GetAddressOf()))
                throw winrt::hresult_error{hr};
            output_buffer.pSample = output_sample.Get();
        }

        DWORD status = 0; // MFT_OUTPUT_STATUS_SAMPLE_READY
        auto hr = transform->ProcessOutput(0, 1, &output_buffer, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
            break;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (output_buffer.dwStatus != MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE) {
                // todo: add more works for this case
                co_return;
            }
            // ...
            if (auto hr = transform->GetOutputAvailableType(output_buffer.dwStreamID, 0, output_type.GetAddressOf()))
                throw winrt::hresult_error{hr};
            // specify the format we want ...
            GUID output_subtype{};
            if (auto hr = output_type->GetGUID(MF_MT_SUBTYPE, &output_subtype))
                throw winrt::hresult_error{hr};

            if (auto hr = transform->SetOutputType(0, output_type.Get(), 0))
                throw winrt::hresult_error{hr};
            continue;
        }
        if (hr != S_OK)
            throw winrt::hresult_error{hr};

        if (output_stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)
            output_sample = output_buffer.pSample;
        co_yield output_sample;
    }
}


HRESULT create_single_buffer_sample(DWORD bufsz, IMFSample** sample) {
    if (auto hr = MFCreateSample(sample))
        return hr;
    ComPtr<IMFMediaBuffer> buffer{};
    if (auto hr = MFCreateMemoryBuffer(bufsz, buffer.GetAddressOf()))
        return hr;
    return (*sample)->AddBuffer(buffer.Get());
    ComPtr<IMFSample> owner{};
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
