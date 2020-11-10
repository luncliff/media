#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.Threading.h>

#include <media.hpp>
#include <spdlog/spdlog.h>

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

auto media_startup() noexcept(false) -> gsl::final_action<HRESULT(WINAPI*)()> {
    if (auto hr = MFStartup(MF_VERSION))
        throw winrt::hresult_error{hr};
    spdlog::info("media_foundation:");
    spdlog::info("- version: {:x}", MF_VERSION);
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

HRESULT make_transform_video(IMFTransform** transform, const IID& iid) noexcept {
    ComPtr<IUnknown> unknown{};
    if (auto hr = CoCreateInstance(iid, NULL, CLSCTX_INPROC_SERVER, IID_IUnknown, (void**)unknown.GetAddressOf()))
        return hr;
    return unknown->QueryInterface(IID_PPV_ARGS(transform));
}

HRESULT make_transform_video(IMFTransform** transform) noexcept {
    return make_transform_video(transform, CLSID_VideoProcessorMFT); 
}



HRESULT configure_video_output_RGB565(IMFMediaType* type) noexcept {
    if (type == nullptr)
        return E_INVALIDARG;
    if (auto hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))
        return hr;
    if (auto hr = type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE))
        return hr;
    return type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB565);
}

HRESULT make_video_output_RGB565(IMFMediaType** ptr) noexcept {
    if (ptr == nullptr)
        return E_INVALIDARG;
    *ptr = nullptr;
    ComPtr<IMFMediaType> output_type{};
    if (auto hr = MFCreateMediaType(output_type.GetAddressOf()))
        return hr;
    if (auto hr = configure_video_output_RGB565(output_type.Get()))
        return hr;
    output_type->AddRef();
    *ptr = output_type.Get();
    return S_OK;
}

HRESULT configure_video_output_RGB32(IMFMediaType* type) noexcept {
    if (type == nullptr)
        return E_INVALIDARG;
    if (auto hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))
        return hr;
    if (auto hr = type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE))
        return hr;
    if (auto hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Unknown))
        return hr;
    //if (auto hr = MFSetAttributeRatio(output_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 16, 9))
    //    return hr;
    return type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
}

HRESULT make_video_output_RGB32(IMFMediaType** ptr) noexcept {
    if (ptr == nullptr)
        return E_INVALIDARG;
    *ptr = nullptr;
    ComPtr<IMFMediaType> output_type{};
    if (auto hr = MFCreateMediaType(output_type.GetAddressOf()))
        return hr;
    if (auto hr = configure_video_output_RGB32(output_type.Get()))
        return hr;
    output_type->AddRef();
    *ptr = output_type.Get();
    return S_OK;
}

HRESULT try_output_type(IMFTransform* transform, DWORD ostream, const GUID& desired,
                        IMFMediaType** output_type) noexcept {
    DWORD type_index = 0;
    for (ComPtr<IMFMediaType> candidate : try_output_available_types(transform, ostream, type_index)) {
        GUID subtype{};
        if (auto hr = candidate->GetGUID(MF_MT_SUBTYPE, &subtype))
            return hr;
        if (subtype != desired)
            continue;
        if (auto hr = transform->SetOutputType(ostream, candidate.Get(), 0))
            return hr;
        break;
    }
    if (type_index == 0)
        return E_FAIL;
    return transform->GetOutputCurrentType(ostream, output_type);
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
        if (flags & MF_SOURCE_READERF_STREAMTICK)
            spdlog::debug("MF_SOURCE_READERF_STREAMTICK");
        if (input_sample == nullptr) // probably MF_SOURCE_READERF_STREAMTICK
            continue;
        co_yield input_sample;
    }
};

auto decode(ComPtr<IMFTransform> transform, ComPtr<IMFMediaType> output_type, //
            DWORD ostream) noexcept(false) -> std::experimental::generator<ComPtr<IMFSample>> {
    MFT_OUTPUT_STREAM_INFO output_stream_info{};
    if (auto hr = transform->GetOutputStreamInfo(ostream, &output_stream_info))
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

auto process(ComPtr<IMFTransform> transform, DWORD istream, DWORD ostream, //
             ComPtr<IMFSample> input_sample, ComPtr<IMFMediaType> output_type, HRESULT& ec) noexcept
    -> std::experimental::generator<ComPtr<IMFSample>> {
    DWORD index{};
    DWORD flags{};
    LONGLONG timestamp{}; // unit 100-nanosecond
    LONGLONG duration{};
    if (ec = input_sample->SetSampleTime(timestamp))
        co_return;

    switch (ec = transform->ProcessInput(istream, input_sample.Get(), 0)) {
    case S_OK: // MF_E_TRANSFORM_TYPE_NOT_SET, MF_E_NO_SAMPLE_DURATION, MF_E_NO_SAMPLE_TIMESTAMP
        break;
    case MF_E_UNSUPPORTED_D3D_TYPE:
    case MF_E_NOTACCEPTING:
    default:
        // error
        co_return;
    }
    // fetch output if available
    for (ComPtr<IMFSample> output_sample : decode(transform, output_type, ostream))
        co_yield output_sample;
}

auto process(ComPtr<IMFTransform> transform, DWORD istream, DWORD ostream, ComPtr<IMFSourceReader> source_reader,
             HRESULT& ec) -> std::experimental::generator<ComPtr<IMFSample>> {
    ComPtr<IMFMediaType> output_type{};
    if (ec = transform->GetOutputCurrentType(ostream, output_type.GetAddressOf()))
        co_return;

    if (ec = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL))
        co_return;
    if (ec = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL))
        co_return;

    DWORD index{};
    DWORD flags{};
    LONGLONG timestamp{}; // unit 100-nanosecond
    LONGLONG duration{};
    for (ComPtr<IMFSample> input_sample : read_samples(source_reader, //
                                                       index, flags, timestamp, duration)) {
        input_sample->SetSampleTime(timestamp);
        for (ComPtr<IMFSample> output_sample : process(transform, istream, ostream, input_sample, output_type, ec))
            co_yield output_sample;
        if (ec)
            co_return;
    }
    if (ec = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, NULL))
        co_return;
    if (ec = transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, NULL))
        co_return;

    for (ComPtr<IMFSample> output_sample : decode(transform, output_type, ostream))
        co_yield output_sample;
    ec = S_OK;
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
