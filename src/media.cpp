#include <media.hpp>
#include <spdlog/spdlog.h>

#include <codecapi.h> // for [codec]
#include <dshowasf.h>
#include <mediaobj.h> // for [dsp]

using namespace std;

// {11790296-A926-45AB-96CB-A9CB187F37AD}
const GUID module_guid_0 = {0x11790296, 0xa926, 0x45ab, {0x96, 0xcb, 0xa9, 0xcb, 0x18, 0x7f, 0x37, 0xad}};

const GUID& get_guid0() noexcept {
    return module_guid_0;
}

auto media_startup() noexcept(false) -> gsl::final_action<HRESULT(WINAPI*)()> {
    winrt::check_hresult(MFStartup(MF_VERSION));
    return gsl::finally(&MFShutdown);
}

HRESULT get_devices(std::vector<com_ptr<IMFActivate>>& devices, IMFAttributes* attributes) noexcept {
    IMFActivate** handles = nullptr;
    UINT32 count = 0;
    if (auto hr = MFEnumDeviceSources(attributes, &handles, &count); FAILED(hr))
        return hr;
    auto on_return = gsl::finally([handles]() {
        CoTaskMemFree(handles); // must be deallocated
    });
    for (auto i = 0u; i < count; ++i) {
        com_ptr<IMFActivate> activate{};
        activate.attach(handles[i]);
        devices.emplace_back(move(activate));
    }
    return S_OK;
}

HRESULT get_string(gsl::not_null<IMFAttributes*> attribute, const GUID& uuid, winrt::hstring& name) noexcept {
    constexpr UINT32 max_size = 240;
    WCHAR buf[max_size]{};
    UINT32 buflen{};
    HRESULT hr = attribute->GetString(uuid, buf, max_size, &buflen);
    if SUCCEEDED (hr)
        name = {buf, buflen};
    return hr;
}

HRESULT get_name(gsl::not_null<IMFActivate*> device, winrt::hstring& name) noexcept {
    return get_string(device, MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, name);
}

HRESULT get_name(gsl::not_null<IMFActivate*> device, std::string& ref) noexcept {
    winrt::hstring name{};
    auto hr = get_name(device, name);
    if SUCCEEDED (hr)
        ref = winrt::to_string(name);
    return hr;
}

HRESULT get_name(gsl::not_null<IMFActivate*> device, std::wstring& ref) noexcept {
    winrt::hstring name{};
    auto hr = get_name(device, name);
    if SUCCEEDED (hr)
        ref = {name.c_str(), name.size()};
    return hr;
}

HRESULT get_hardware_url(gsl::not_null<IMFTransform*> transform, winrt::hstring& name) noexcept {
    com_ptr<IMFAttributes> attrs{};
    if (auto hr = transform->GetAttributes(attrs.put()); FAILED(hr))
        return hr;
    constexpr UINT32 max_size = 240;
    WCHAR buf[max_size]{};
    UINT32 buflen{};
    HRESULT hr = attrs->GetString(MFT_ENUM_HARDWARE_URL_Attribute, buf, max_size, &buflen);
    if SUCCEEDED (hr)
        name = {buf, buflen};
    return hr;
}

HRESULT resolve(const fs::path& fpath, IMFMediaSourceEx** source, MF_OBJECT_TYPE& media_object_type) noexcept {
    com_ptr<IMFSourceResolver> resolver{};
    if (auto hr = MFCreateSourceResolver(resolver.put()); FAILED(hr))
        return hr;
    com_ptr<IUnknown> unknown{};
    if (auto hr = resolver->CreateObjectFromURL(fpath.c_str(), MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_READ, NULL,
                                                &media_object_type, unknown.put());
        FAILED(hr))
        return hr;
    return unknown->QueryInterface(source);
}

// todo: allow process server
HRESULT make_transform_video(IMFTransform** transform, const IID& iid) noexcept {
    com_ptr<IUnknown> unknown{};
    if (auto hr = CoCreateInstance(iid, NULL, CLSCTX_ALL, IID_PPV_ARGS(unknown.put())); FAILED(hr))
        return hr;
    return unknown->QueryInterface(transform);
}

HRESULT configure_D3D11_DXGI(gsl::not_null<IMFTransform*> transform,
                             gsl::not_null<IMFDXGIDeviceManager*> device_manager) noexcept {
    com_ptr<IMFAttributes> attrs{};
    if (auto hr = transform->GetAttributes(attrs.put()); FAILED(hr)) // return can be E_NOTIMPL
        return hr;
    UINT32 supported{};
    if (auto hr = attrs->GetUINT32(MF_SA_D3D11_AWARE, &supported); FAILED(hr))
        return hr;
    if (supported == false)
        return E_FAIL;
    return transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, //
                                     reinterpret_cast<ULONG_PTR>(static_cast<IUnknown*>(device_manager)));
}

// @see https://docs.microsoft.com/en-us/windows/win32/medfound/h-264-video-decoder#transform-attributes
HRESULT configure_acceleration_H264(gsl::not_null<IMFTransform*> transform) noexcept {
    com_ptr<IMFAttributes> attrs{};
    if (auto hr = transform->GetAttributes(attrs.put()); FAILED(hr))
        return hr;
    if (auto hr = attrs->SetUINT32(CODECAPI_AVDecVideoAcceleration_H264, TRUE); FAILED(hr))
        spdlog::error("CODECAPI_AVDecVideoAcceleration_H264: {:#08x}", hr);
    if (auto hr = attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE); FAILED(hr))
        spdlog::error("CODECAPI_AVLowLatencyMode: {:#08x}", hr);
    if (auto hr = attrs->SetUINT32(CODECAPI_AVDecNumWorkerThreads, 1); FAILED(hr))
        spdlog::error("CODECAPI_AVDecNumWorkerThreads: {:#08x}", hr);
    return S_OK;
}

HRESULT make_video_type(gsl::not_null<IMFMediaType**> ptr, const GUID& subtype) noexcept {
    com_ptr<IMFMediaType> type{};
    if (auto hr = MFCreateMediaType(type.put()); FAILED(hr))
        return hr;
    if (auto hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); FAILED(hr))
        return hr;
    if (auto hr = type->SetGUID(MF_MT_SUBTYPE, subtype); FAILED(hr))
        return hr;
    if (IUnknown* unknown = *ptr = type.get())
        unknown->AddRef();
    return S_OK;
}

HRESULT make_video_RGB565(gsl::not_null<IMFMediaType**> ptr) noexcept {
    if (auto hr = make_video_type(ptr, MFVideoFormat_RGB565); FAILED(hr))
        return hr;
    IMFMediaType* type = *ptr;
    UNREFERENCED_PARAMETER(type);
    //if (auto hr = type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE); FAILED(hr))
    //    return hr;
    return S_OK;
}

HRESULT make_video_RGB32(gsl::not_null<IMFMediaType**> ptr) noexcept {
    if (auto hr = make_video_type(ptr, MFVideoFormat_RGB32); FAILED(hr))
        return hr;
    IMFMediaType* type = *ptr;
    if (auto hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Unknown); FAILED(hr))
        return hr;
    //if (auto hr = type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE); FAILED(hr))
    //    return hr;
    return S_OK;
}

HRESULT try_output_type(com_ptr<IMFTransform> transform, DWORD ostream, const GUID& desired,
                        IMFMediaType** output_type) noexcept {
    DWORD type_index = 0;
    for (com_ptr<IMFMediaType> candidate : try_output_available_types(transform, ostream, type_index)) {
        GUID subtype{};
        if (auto hr = candidate->GetGUID(MF_MT_SUBTYPE, &subtype); FAILED(hr))
            return hr;
        if (subtype != desired)
            continue;
        if (auto hr = transform->SetOutputType(ostream, candidate.get(), 0); FAILED(hr))
            return hr;
        break;
    }
    if (type_index == 0)
        return E_FAIL;
    return transform->GetOutputCurrentType(ostream, output_type);
}

auto get_input_available_types(com_ptr<IMFTransform> transform, DWORD num_input, HRESULT& ec) noexcept(false)
    -> generator<com_ptr<IMFMediaType>> {
    for (auto stream_id = 0u; stream_id < num_input; ++stream_id) {
        DWORD type_index = 0;
        com_ptr<IMFMediaType> media_type{};
        for (ec = transform->GetInputAvailableType(stream_id, type_index++, media_type.put()); SUCCEEDED(ec);
             ec = transform->GetInputAvailableType(stream_id, type_index++, media_type.put())) {
            co_yield media_type;
            media_type = nullptr;
        }
    }
}
auto try_output_available_types(com_ptr<IMFTransform> transform, DWORD stream_id, DWORD& type_index) noexcept(false)
    -> generator<com_ptr<IMFMediaType>> {
    type_index = 0;
    com_ptr<IMFMediaType> media_type{};
    for (auto hr = transform->GetOutputAvailableType(stream_id, type_index++, media_type.put()); SUCCEEDED(hr);
         hr = transform->GetOutputAvailableType(stream_id, type_index++, media_type.put())) {
        co_yield media_type;
        media_type = nullptr;
    }
}

auto get_output_available_types(com_ptr<IMFTransform> transform, DWORD num_output, HRESULT& ec) noexcept(false)
    -> generator<com_ptr<IMFMediaType>> {
    for (auto stream_id = 0u; stream_id < num_output; ++stream_id) {
        DWORD type_index = 0;
        com_ptr<IMFMediaType> media_type{};
        for (ec = transform->GetOutputAvailableType(stream_id, type_index++, media_type.put()); SUCCEEDED(ec);
             ec = transform->GetOutputAvailableType(stream_id, type_index++, media_type.put())) {
            co_yield media_type;
            media_type = nullptr;
        }
    }
}
auto try_input_available_types(com_ptr<IMFTransform> transform, DWORD stream_id, DWORD& type_index) noexcept(false)
    -> generator<com_ptr<IMFMediaType>> {
    type_index = 0;
    com_ptr<IMFMediaType> media_type{};
    for (auto hr = transform->GetInputAvailableType(stream_id, type_index++, media_type.put()); SUCCEEDED(hr);
         hr = transform->GetInputAvailableType(stream_id, type_index++, media_type.put())) {
        co_yield media_type;
        media_type = nullptr;
    }
}

HRESULT configure_rectangle(gsl::not_null<IMFVideoProcessorControl*> control,
                            gsl::not_null<IMFMediaType*> media_type) noexcept {
    UINT32 w = 0, h = 0;
    if (auto hr = MFGetAttributeSize(media_type, MF_MT_FRAME_SIZE, &w, &h); FAILED(hr))
        return hr;
    RECT rect{};
    rect.right = w; // LTRB rectangle
    rect.bottom = h;
    if (auto hr = control->SetSourceRectangle(&rect); FAILED(hr))
        return hr;
    return control->SetDestinationRectangle(&rect);
}

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/videoresizer
HRESULT configure_source_rectangle(gsl::not_null<IPropertyStore*> props, const RECT& rect) noexcept {
    PROPVARIANT val{};
    val.intVal = rect.left;
    if (auto hr = props->SetValue(MFPKEY_RESIZE_SRC_LEFT, val); FAILED(hr))
        return hr;
    val.intVal = rect.top;
    if (auto hr = props->SetValue(MFPKEY_RESIZE_SRC_TOP, val); FAILED(hr))
        return hr;
    val.intVal = rect.right - rect.left;
    if (auto hr = props->SetValue(MFPKEY_RESIZE_SRC_WIDTH, val); FAILED(hr))
        return hr;
    val.intVal = rect.bottom - rect.top;
    return props->SetValue(MFPKEY_RESIZE_SRC_HEIGHT, val);
}

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/videoresizer
HRESULT configure_destination_rectangle(gsl::not_null<IPropertyStore*> props, const RECT& rect) noexcept {
    PROPVARIANT val{};
    val.intVal = rect.left;
    if (auto hr = props->SetValue(MFPKEY_RESIZE_DST_LEFT, val); FAILED(hr))
        return hr;
    val.intVal = rect.top;
    if (auto hr = props->SetValue(MFPKEY_RESIZE_DST_TOP, val); FAILED(hr))
        return hr;
    val.intVal = rect.right - rect.left;
    if (auto hr = props->SetValue(MFPKEY_RESIZE_DST_WIDTH, val); FAILED(hr))
        return hr;
    val.intVal = rect.bottom - rect.top;
    return props->SetValue(MFPKEY_RESIZE_DST_HEIGHT, val);
}

HRESULT create_sink_writer(IMFSinkWriterEx** writer, const fs::path& fpath) noexcept {
    com_ptr<IMFAttributes> attrs{};
    if (auto hr = MFCreateAttributes(attrs.put(), 2); FAILED(hr))
        return hr;
    if (auto hr = attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE); FAILED(hr))
        return hr;
    if (auto hr = attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, FALSE); FAILED(hr))
        return hr;

    com_ptr<IMFSinkWriter> sink_writer{};
    if (auto hr = MFCreateSinkWriterFromURL(fpath.c_str(), nullptr, attrs.get(), sink_writer.put()); FAILED(hr))
        return hr;
    return sink_writer->QueryInterface(writer);
}

HRESULT create_source_reader(com_ptr<IMFMediaSource> source, com_ptr<IMFSourceReaderCallback> callback,
                             IMFSourceReader** reader) noexcept {
    com_ptr<IMFPresentationDescriptor> presentation{};
    if (auto hr = source->CreatePresentationDescriptor(presentation.put()); FAILED(hr))
        return hr;
    com_ptr<IMFStreamDescriptor> stream{};
    if (auto hr = get_stream_descriptor(presentation.get(), stream.put()); FAILED(hr))
        return hr;
    if (auto hr = configure(stream); FAILED(hr))
        return hr;

    com_ptr<IMFAttributes> attrs{};
    if (auto hr = MFCreateAttributes(attrs.put(), 3); FAILED(hr))
        return hr;
    if (callback) {
        if (auto hr = attrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback.get()); FAILED(hr))
            return hr;
    }
    if (auto hr = attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE); FAILED(hr))
        return hr;
    if (auto hr = attrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE); FAILED(hr))
        return hr;
    return MFCreateSourceReaderFromMediaSource(source.get(), attrs.get(), reader);
}

HRESULT get_stream_descriptor(gsl::not_null<IMFPresentationDescriptor*> presentation,
                              IMFStreamDescriptor** ptr) noexcept {
    if (ptr == nullptr)
        return E_INVALIDARG;
    *ptr = nullptr;

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

HRESULT configure_video(com_ptr<IMFMediaType> type) {
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
HRESULT configure(com_ptr<IMFStreamDescriptor> stream) noexcept {
    com_ptr<IMFMediaTypeHandler> handler{};
    if (auto hr = stream->GetMediaTypeHandler(handler.put()); SUCCEEDED(hr) == false)
        return hr;
    DWORD num_types = 0;
    if (auto hr = handler->GetMediaTypeCount(&num_types); SUCCEEDED(hr) == false)
        return hr;
    com_ptr<IMFMediaType> type{};
    for (auto i = 0u; i < num_types; ++i) {
        com_ptr<IMFMediaType> current{};
        if (auto hr = handler->GetMediaTypeByIndex(i, current.put()); FAILED(hr))
            return hr;
        if (type == nullptr)
            type = current;
        //print(current.get());
    }
    return handler->SetCurrentMediaType(type.get());
}

auto read_samples(com_ptr<IMFSourceReader> source_reader, DWORD& stream_index, DWORD& flags,
                  LONGLONG& timestamp) noexcept(false) -> generator<com_ptr<IMFSample>> {
    const auto reader_stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    while (true) {
        com_ptr<IMFSample> input_sample{};
        if (auto hr = source_reader->ReadSample(reader_stream, 0, //
                                                &stream_index, &flags, &timestamp, input_sample.put());
            FAILED(hr))
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

auto decode(com_ptr<IMFTransform> transform, DWORD ostream, com_ptr<IMFMediaType> output_type, HRESULT& ec) noexcept
    -> generator<com_ptr<IMFSample>> {
    MFT_OUTPUT_STREAM_INFO output_stream_info{};
    if (ec = transform->GetOutputStreamInfo(ostream, &output_stream_info); FAILED(ec))
        co_return;

    while (true) {
        MFT_OUTPUT_DATA_BUFFER output_buffer{};
        com_ptr<IMFSample> output_sample{};
        if (output_stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) {
            // ...
        } else {
            if (ec = create_single_buffer_sample(output_sample.put(), output_stream_info.cbSize); FAILED(ec))
                co_return;
            output_buffer.pSample = output_sample.get();
        }

        DWORD status = 0; // MFT_OUTPUT_STATUS_SAMPLE_READY
        ec = transform->ProcessOutput(0, 1, &output_buffer, &status);
        if (ec == MF_E_TRANSFORM_NEED_MORE_INPUT)
            break;
        if (ec == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (output_buffer.dwStatus != MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE) {
                // todo: add more works for this case
                co_return;
            }
            // the type is changed. update after reset
            output_type = nullptr;
            if (ec = transform->GetOutputAvailableType(output_buffer.dwStreamID, 0, output_type.put()); FAILED(ec))
                co_return;
            // specify the format we want ...
            GUID output_subtype{};
            if (ec = output_type->GetGUID(MF_MT_SUBTYPE, &output_subtype); FAILED(ec))
                co_return;
            if (ec = transform->SetOutputType(ostream, output_type.get(), 0); FAILED(ec))
                co_return;
            continue;
        }
        if (ec != S_OK)
            co_return;

        if (output_stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)
            output_sample.attach(output_buffer.pSample);
        co_yield output_sample;
    }
}

auto process(com_ptr<IMFTransform> transform, DWORD istream, DWORD ostream, //
             com_ptr<IMFSample> input_sample, com_ptr<IMFMediaType> output_type, HRESULT& ec) noexcept
    -> generator<com_ptr<IMFSample>> {
    DWORD index{};
    DWORD flags{};
    LONGLONG timestamp{}; // unit 100-nanosecond
    if (ec = input_sample->SetSampleTime(timestamp); FAILED(ec))
        co_return;

    switch (ec = transform->ProcessInput(istream, input_sample.get(), 0)) {
    case S_OK: // MF_E_TRANSFORM_TYPE_NOT_SET, MF_E_NO_SAMPLE_DURATION, MF_E_NO_SAMPLE_TIMESTAMP
        break;
    case MF_E_UNSUPPORTED_D3D_TYPE:
    case MF_E_NOTACCEPTING:
    default:
        // error
        co_return;
    }
    // fetch output if available
    for (com_ptr<IMFSample> output_sample : decode(transform, ostream, output_type, ec))
        co_yield output_sample;
}

auto process(com_ptr<IMFTransform> transform, DWORD istream, DWORD ostream, com_ptr<IMFSourceReader> source_reader,
             HRESULT& ec) -> generator<com_ptr<IMFSample>> {
    com_ptr<IMFMediaType> output_type{};
    if (ec = transform->GetOutputCurrentType(ostream, output_type.put()); FAILED(ec))
        co_return;
    if (ec = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL); FAILED(ec))
        co_return;
    if (ec = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL); FAILED(ec))
        co_return;

    DWORD index{};
    DWORD flags{};
    LONGLONG timestamp{}; // unit 100-nanosecond
    for (com_ptr<IMFSample> input_sample : read_samples(source_reader, index, flags, timestamp)) {
        input_sample->SetSampleTime(timestamp);
        for (com_ptr<IMFSample> output_sample : process(transform, istream, ostream, input_sample, output_type, ec))
            co_yield output_sample;
        if FAILED (ec)
            co_return;
    }
    if (ec = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, NULL); FAILED(ec))
        co_return;
    if (ec = transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, NULL); FAILED(ec))
        co_return;

    for (com_ptr<IMFSample> output_sample : decode(transform, ostream, output_type, ec))
        co_yield output_sample;
}

HRESULT create_single_buffer_sample(IMFSample** sample, DWORD bufsz) {
    if (auto hr = MFCreateSample(sample))
        return hr;
    com_ptr<IMFMediaBuffer> buffer{};
    if (auto hr = MFCreateMemoryBuffer(bufsz, buffer.put()))
        return hr;

    //DWORD cap = 0;
    //if (auto hr = buffer->GetMaxLength(&cap))
    //    return hr;
    //DWORD len = 0;
    //if (auto hr = buffer->GetCurrentLength(&len))
    //    return hr;
    return (*sample)->AddBuffer(buffer.get());
}

HRESULT create_and_copy_single_buffer_sample(IMFSample* src, IMFSample** dst) {
    DWORD total{};
    if (auto hr = src->GetTotalLength(&total); FAILED(hr))
        return hr;
    if (auto hr = create_single_buffer_sample(dst, total); FAILED(hr))
        return hr;
    if (auto hr = src->CopyAllItems(*dst); FAILED(hr))
        return hr;
    com_ptr<IMFMediaBuffer> buffer{};
    if (auto hr = (*dst)->GetBufferByIndex(0, buffer.put()); FAILED(hr))
        return hr;
    return src->CopyToBuffer(buffer.get());
}

HRESULT get_transform_output(IMFTransform* transform, IMFSample** sample, BOOL& flushed) {
    MFT_OUTPUT_STREAM_INFO stream_info{};
    if (auto hr = transform->GetOutputStreamInfo(0, &stream_info); FAILED(hr))
        return hr;

    flushed = FALSE;
    *sample = nullptr;

    MFT_OUTPUT_DATA_BUFFER output{};
    if ((stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
        if (auto hr = create_single_buffer_sample(sample, stream_info.cbSize); FAILED(hr))
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
        com_ptr<IMFMediaType> changed_output_type{};
        if (output.dwStatus != MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE) {
            // todo: add more works for this case
            return E_NOTIMPL;
        }

        if (auto hr = transform->GetOutputAvailableType(0, 0, changed_output_type.put()); FAILED(hr))
            return hr;

        // check new output media type

        if (auto hr = changed_output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_IYUV); FAILED(hr))
            return hr;
        if (auto hr = transform->SetOutputType(0, changed_output_type.get(), 0); FAILED(hr))
            return hr;

        if (auto hr = transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL); FAILED(hr))
            return hr;
        flushed = TRUE;

        return S_OK;
    }
    // MF_E_TRANSFORM_NEED_MORE_INPUT: not an error condition but it means the allocated output sample is empty.
    return result;
}
