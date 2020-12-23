/**
 * @file transform_test.cpp
 * @author github.com/luncliff (luncliff@gmail.com)
 */
#define CATCH_CONFIG_WCHAR
#define CATCH_CONFIG_WINDOWS_CRTDBG
#include <catch2/catch.hpp>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.Threading.h>

#include <filesystem>
#include <media.hpp>

#include <codecapi.h> // for [codec]
#include <mediaobj.h> // for [dsp]
#include <mmdeviceapi.h>
#include <wmsdkidl.h>

#include <spdlog/spdlog.h>

using namespace std;
namespace fs = std::filesystem;

fs::path get_asset_dir() noexcept;

// there might be no device in test environment. if the case, it's an expected failure
TEST_CASE("IMFActivate(VideoCapture)", "[!mayfail]") {
    auto on_return = media_startup();

    std::vector<com_ptr<IMFActivate>> devices{};
    {
        com_ptr<IMFAttributes> attrs{};
        REQUIRE(MFCreateAttributes(attrs.put(), 1) == S_OK);
        REQUIRE(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID) ==
                S_OK);
        REQUIRE(get_devices(devices, attrs.get()) == S_OK);
    }
    REQUIRE(devices.size());

    for (com_ptr<IMFActivate> device : devices) {
        com_ptr<IMFMediaSourceEx> source{};
        if (auto hr = device->ActivateObject(__uuidof(IMFMediaSourceEx), source.put_void()))
            FAIL(hr);

        com_ptr<IMFAttributes> attrs{};
        if (auto hr = source->GetSourceAttributes(attrs.put()))
            FAIL(hr);

        GUID capture{};
        if (auto hr = attrs->GetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &capture))
            FAIL(hr);
        REQUIRE(capture == MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

        print(device.get());
    }
}

TEST_CASE("IMFSourceResolver") {
    const auto fpath = fs::absolute(get_asset_dir() / "fm5p7flyCSY.mp4");
    REQUIRE(fs::exists(fpath));

    auto on_return = media_startup();

    com_ptr<IMFSourceResolver> resolver{};
    REQUIRE(MFCreateSourceResolver(resolver.put()) == S_OK);

    com_ptr<IUnknown> unknown{};
    MF_OBJECT_TYPE media_object_type{};
    if (auto hr = resolver->CreateObjectFromURL(fpath.c_str(), MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_READ, NULL,
                                                &media_object_type, unknown.put()))
        FAIL(hr); // MF_E_UNSUPPORTED_SCHEME, MF_E_SOURCERESOLVER_MUTUALLY_EXCLUSIVE_FLAGS
    REQUIRE(media_object_type == MF_OBJECT_MEDIASOURCE);

    SECTION("IMFMediaSource") {
        com_ptr<IMFMediaSource> source{};
        REQUIRE(unknown->QueryInterface(source.put()) == S_OK);
    }
    SECTION("IMFMediaSourceEx") {
        com_ptr<IMFMediaSourceEx> source{};
        REQUIRE(unknown->QueryInterface(source.put()) == S_OK);
    }
}

TEST_CASE("MFCreateSourceReader", "[!mayfail]") {
    auto on_return = media_startup();

    com_ptr<IMFAttributes> attributes{};
    REQUIRE(MFCreateAttributes(attributes.put(), 2) == S_OK);

    const auto fpath = get_asset_dir() / "fm5p7flyCSY.mp4";
    SECTION("URL") {
        com_ptr<IMFSourceReader> reader{};
        REQUIRE(MFCreateSourceReaderFromURL(fpath.c_str(), nullptr, reader.put()) == S_OK);
        com_ptr<IMFSourceReaderEx> reader2{};
        REQUIRE(reader->QueryInterface(reader2.put()) == S_OK);
    }
    SECTION("IMFMediaSource") {
        com_ptr<IMFMediaSourceEx> source{};
        MF_OBJECT_TYPE media_object_type = MF_OBJECT_INVALID;
        REQUIRE(resolve(fpath, source.put(), media_object_type) == S_OK);

        com_ptr<IMFSourceReader> reader{};
        REQUIRE(MFCreateSourceReaderFromMediaSource(source.get(), nullptr, reader.put()) == S_OK);
    }
    SECTION("IMFByteStream") {
        com_ptr<IMFByteStream> byte_stream{};
        if (auto ec = MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE, fpath.c_str(),
                                   byte_stream.put()))
            FAIL(ec);

        com_ptr<IMFSourceReader> reader{};
        REQUIRE(MFCreateSourceReaderFromByteStream(byte_stream.get(), nullptr, reader.put()) == S_OK);
    }
}

TEST_CASE("IMFSourceReaderEx(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING)") {
    auto on_return = media_startup();

    com_ptr<IMFMediaSourceEx> source{};
    MF_OBJECT_TYPE media_object_type = MF_OBJECT_INVALID;
    REQUIRE(resolve(get_asset_dir() / "fm5p7flyCSY.mp4", source.put(), media_object_type) == S_OK);

    com_ptr<IMFSourceReaderEx> source_reader{};
    {
        com_ptr<IMFAttributes> attrs{};
        REQUIRE(MFCreateAttributes(attrs.put(), 2) == S_OK);
        REQUIRE(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE) == S_OK);
        REQUIRE(attrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE) == S_OK);
        com_ptr<IMFSourceReader> reader{};
        REQUIRE(MFCreateSourceReaderFromMediaSource(source.get(), attrs.get(), reader.put()) == S_OK);
        REQUIRE(reader->QueryInterface(source_reader.put()) == S_OK);
    }

    // https://docs.microsoft.com/en-us/windows/win32/medfound/mf-source-reader-enable-advanced-video-processing#remarks
    com_ptr<IMFMediaType> source_type{};
    DWORD stream = MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    REQUIRE(source_reader->GetCurrentMediaType(stream, source_type.put()) == S_OK);

    // make a copy instead of modification
    com_ptr<IMFMediaType> output_type{};
    REQUIRE(MFCreateMediaType(output_type.put()) == S_OK);
    REQUIRE(source_type->CopyAllItems(output_type.get()) == S_OK);

    SECTION("MF_MT_SUBTYPE(MFVideoFormat_NV12)") {
        REQUIRE(output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12) == S_OK);
        REQUIRE(source_reader->SetCurrentMediaType(stream, NULL, output_type.get()) == S_OK);
    }
    SECTION("MF_MT_SUBTYPE(MFVideoFormat_RGB32)") {
        REQUIRE(output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32) == S_OK);
        REQUIRE(source_reader->SetCurrentMediaType(stream, NULL, output_type.get()) == S_OK);
    }
    // The followings may not pass. Will be resolved in future
    SECTION("MF_MT_INTERLACE_MODE") {
        REQUIRE(output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Unknown) == S_OK);
        const auto hr = source_reader->SetCurrentMediaType(stream, NULL, output_type.get());
        REQUIRE(FAILED(hr));
    }
    SECTION("MF_MT_FRAME_RATE/MF_MT_FRAME_SIZE") {
        REQUIRE(MFSetAttributeRatio(output_type.get(), MF_MT_FRAME_RATE, 30, 1) == S_OK);    // 30 fps
        REQUIRE(MFSetAttributeSize(output_type.get(), MF_MT_FRAME_SIZE, 1280, 720) == S_OK); // w 1280 h 720
        const auto hr = source_reader->SetCurrentMediaType(stream, NULL, output_type.get());
        REQUIRE(FAILED(hr));
    }
}

/// @todo https://docs.microsoft.com/en-us/windows/win32/medfound/mf-source-reader-enable-advanced-video-processing#remarks
void make_test_source(com_ptr<IMFMediaSourceEx>& source, com_ptr<IMFSourceReader>& source_reader,
                      com_ptr<IMFMediaType>& source_type, //
                      const GUID& output_subtype, const fs::path& fpath) {
    MF_OBJECT_TYPE source_object_type = MF_OBJECT_INVALID;
    REQUIRE(resolve(fpath, source.put(), source_object_type) == S_OK);
    {
        com_ptr<IMFAttributes> attrs{};
        REQUIRE(MFCreateAttributes(attrs.put(), 2) == S_OK);
        REQUIRE(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE) == S_OK);
        REQUIRE(attrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE) == S_OK);
        REQUIRE(MFCreateSourceReaderFromMediaSource(source.get(), attrs.get(), source_reader.put()) == S_OK);
    }
    DWORD stream = MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    REQUIRE(source_reader->GetCurrentMediaType(stream, source_type.put()) == S_OK);
    REQUIRE(source_type->SetGUID(MF_MT_SUBTYPE, output_subtype) == S_OK);
    switch (auto hr = source_reader->SetCurrentMediaType(stream, NULL, source_type.get())) {
    case S_OK:
        break;
    case MF_E_INVALIDMEDIATYPE:
        FAIL("MF_E_INVALIDMEDIATYPE");
    default:
        FAIL(hr);
    }
}

HRESULT check_sample(com_ptr<IMFSample> sample) {
    com_ptr<IMFMediaBuffer> buffer{};
    {
        DWORD num_buffer = 0;
        if (auto hr = sample->GetBufferCount(&num_buffer))
            return hr;
        if (num_buffer > 1) {
            if (auto hr = sample->ConvertToContiguousBuffer(buffer.put()))
                return hr;
        } else {
            sample->GetBufferByIndex(0, buffer.put());
        }
    }
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

    com_ptr<IMFMediaSourceEx> source{};
    com_ptr<IMFSourceReader> source_reader{};
    com_ptr<IMFMediaType> source_type{};
    make_test_source(source, source_reader, source_type, MFVideoFormat_H264, get_asset_dir() / "fm5p7flyCSY.mp4");

    com_ptr<IMFTransform> transform{};
    REQUIRE(make_transform_H264(transform.put()) == S_OK);
    REQUIRE(configure_acceleration_H264(transform.get()) == S_OK);
    print(transform.get(), CLSID_CMSH264DecoderMFT); // this may modifies input/output configuration

    // Valid configuration order can be I->O or O->I. `CLSID_CMSH264DecoderMFT` uses I->O ordering
    const DWORD istream = 0, ostream = 0;
    const GUID desired_subtype = MFVideoFormat_IYUV;
    {
        REQUIRE(transform->SetInputType(istream, source_type.get(), 0) == S_OK);
        com_ptr<IMFMediaType> output{};
        REQUIRE(MFCreateMediaType(output.put()) == S_OK);
        REQUIRE(source_type->CopyAllItems(output.get()) == S_OK);

        REQUIRE(output->SetGUID(MF_MT_SUBTYPE, desired_subtype) == S_OK);
        REQUIRE(transform->SetOutputType(ostream, output.get(), 0) == S_OK);

        DWORD status = 0;
        REQUIRE(transform->GetInputStatus(istream, &status) == S_OK);
        REQUIRE(status == MFT_INPUT_STATUS_ACCEPT_DATA);
    }
    // for Asynchronous MFT
    REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL) == S_OK);
    // @todo https://docs.microsoft.com/en-us/windows/win32/medfound/basic-mft-processing-model#get-buffer-requirements
    // ...
    // @see https://docs.microsoft.com/en-us/windows/win32/medfound/basic-mft-processing-model#process-data
    // all types are configured. prepare for upcoming processing
    REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL) == S_OK);

    SECTION("Synchronous(Simplified)") {
        INFO("testing synchronous read/transform with simplified code");
        com_ptr<IMFMediaType> output_type{};
        if (auto hr = transform->GetOutputCurrentType(ostream, output_type.put()))
            FAIL(hr);

        size_t count = 0;
        DWORD index{};
        DWORD flags{};
        LONGLONG timestamp{}; // unit 100-nanosecond
        LONGLONG duration{};
        for (com_ptr<IMFSample> input_sample : read_samples(source_reader, //
                                                            index, flags, timestamp, duration)) {
            switch (auto hr = transform->ProcessInput(istream, input_sample.get(), 0)) {
            case S_OK: // MF_E_TRANSFORM_TYPE_NOT_SET, MF_E_NO_SAMPLE_DURATION, MF_E_NO_SAMPLE_TIMESTAMP
                break;
            case MF_E_NOTACCEPTING:
            case MF_E_UNSUPPORTED_D3D_TYPE:
            case E_INVALIDARG:
            default:
                FAIL(hr);
            }
            HRESULT ec = S_OK;
            for (com_ptr<IMFSample> output_sample : decode(transform, ostream, output_type, ec)) {
                if (auto hr = check_sample(output_sample))
                    FAIL(hr);
                ++count;
            }
            switch (ec) {
            case S_OK:
            case MF_E_TRANSFORM_NEED_MORE_INPUT:
                continue;
            default:
                FAIL(ec);
            }
        }
        REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, NULL) == S_OK);
        REQUIRE(transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, NULL) == S_OK);
        REQUIRE(count);
        count = 0;
        // fetch remaining output in the transform
        HRESULT ec = S_OK;
        for (com_ptr<IMFSample> output_sample : decode(transform, ostream, output_type, ec)) {
            if (auto hr = check_sample(output_sample))
                FAIL(hr);
            ++count;
        }
        switch (ec) {
        case S_OK:
        case MF_E_TRANSFORM_NEED_MORE_INPUT:
            break;
        default:
            FAIL(ec);
        }
        REQUIRE(count);
    }

    SECTION("Synchronous(Detailed)") {
        INFO("testing synchronous read/transform");
        bool input_available = true;
        while (input_available) {
            DWORD stream_index{};
            DWORD sample_flags{};
            LONGLONG sample_timestamp = 0; // unit 100-nanosecond
            LONGLONG sample_duration = 0;
            com_ptr<IMFSample> input_sample{};
            if (auto hr = source_reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream_index,
                                                    &sample_flags, &sample_timestamp, input_sample.put())) {
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
            switch (auto hr = transform->ProcessInput(istream, input_sample.get(), 0)) {
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

                com_ptr<IMFSample> output_sample{};
                if (stream.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) {
                    // ...
                } else {
                    if (auto hr = create_single_buffer_sample(output_sample.put(), stream.cbSize))
                        FAIL(hr);
                    output.pSample = output_sample.get();
                }
                auto hr = transform->ProcessOutput(0, 1, &output, &status);
                if (hr == S_OK)
                    continue;
                if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
                    break;
                if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                    com_ptr<IMFMediaType> output_type{};
                    if (output.dwStatus != MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE) {
                        // todo: add more works for this case
                        FAIL("MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE");
                    }
                    // ...
                    output_type = nullptr;
                    if (auto hr = transform->GetOutputAvailableType(output.dwStreamID, 0, output_type.put()))
                        FAIL(hr);
                    // specify the format we want ...
                    if (auto hr = output_type->SetGUID(MF_MT_SUBTYPE, desired_subtype))
                        FAIL(hr);
                    if (auto hr = transform->SetOutputType(0, output_type.get(), 0))
                        FAIL(hr);
                    continue;
                }
                if (hr == E_FAIL)
                    spdlog::error("transform->ProcessOutput");
                FAIL(hr);
            }
        }
        REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, NULL) == S_OK);
        REQUIRE(transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, NULL) == S_OK);
        // fetch remaining output in the transform
    }
}

// see https://docs.microsoft.com/en-us/windows/win32/medfound/colorconverter
// see https://docs.microsoft.com/en-us/windows/win32/medfound/basic-mft-processing-model
TEST_CASE("MFTransform - Color Converter DSP", "[dsp]") {
    auto on_return = media_startup();

    com_ptr<IMFMediaSourceEx> source{};
    com_ptr<IMFSourceReader> source_reader{};
    com_ptr<IMFMediaType> source_type{};
    make_test_source(source, source_reader, source_type, MFVideoFormat_H264, get_asset_dir() / "fm5p7flyCSY.mp4");

    com_ptr<IMFTransform> transform{};
    REQUIRE(make_transform_video(transform.put(), CLSID_CColorConvertDMO) == S_OK);
    com_ptr<IPropertyStore> props{};
    {
        REQUIRE(transform->QueryInterface(props.put()) == S_OK);
        PROPVARIANT var{};
        REQUIRE(SUCCEEDED(props->GetValue(MFPKEY_COLORCONV_MODE, &var)));
        spdlog::debug("- MFPKEY_COLORCONV_MODE: {}", var.intVal == 0 ? "Progressive" : "Interlaced");
        // Microsoft DirectX Media Object https://docs.microsoft.com/en-us/previous-versions/windows/desktop/api/mediaobj/nn-mediaobj-imediaobject
        com_ptr<IMediaObject> media_object{};
        REQUIRE(transform->QueryInterface(media_object.put()) == S_OK);
    }

    auto make_output_RGB32 = [](com_ptr<IMFMediaType> input) -> com_ptr<IMFMediaType> {
        com_ptr<IMFMediaType> output{};
        REQUIRE(make_video_output_RGB32(output.put()) == S_OK);
        UINT32 w = 0, h = 0;
        REQUIRE(MFGetAttributeSize(input.get(), MF_MT_FRAME_SIZE, &w, &h) == S_OK);
        REQUIRE(MFSetAttributeSize(output.get(), MF_MT_FRAME_SIZE, w, h) == S_OK);
        UINT32 num = 0, denom = 1;
        REQUIRE(MFGetAttributeRatio(input.get(), MF_MT_FRAME_RATE, &num, &denom) == S_OK);
        REQUIRE(MFSetAttributeSize(output.get(), MF_MT_FRAME_RATE, num, denom) == S_OK);
        return output;
    };
    auto make_output_RGB565 = [](com_ptr<IMFMediaType> input) -> com_ptr<IMFMediaType> {
        com_ptr<IMFMediaType> output{};
        REQUIRE(make_video_output_RGB565(output.put()) == S_OK);
        UINT32 w = 0, h = 0;
        REQUIRE(MFGetAttributeSize(input.get(), MF_MT_FRAME_SIZE, &w, &h) == S_OK);
        REQUIRE(MFSetAttributeSize(output.get(), MF_MT_FRAME_SIZE, w, h) == S_OK);
        UINT32 num = 0, denom = 1;
        REQUIRE(MFGetAttributeRatio(input.get(), MF_MT_FRAME_RATE, &num, &denom) == S_OK);
        REQUIRE(MFSetAttributeSize(output.get(), MF_MT_FRAME_RATE, num, denom) == S_OK);
        return output;
    };

    SECTION("MP4(NV12) - RGB32") {
        INFO("IMFMediaSourceEx: MP4(NV12) -> RGB32");
        REQUIRE(source_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12) == S_OK);
        REQUIRE(source_reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, source_type.get()) ==
                S_OK);
        print(source_type.get());
        if (auto hr = transform->SetInputType(0, source_type.get(), 0))
            FAIL(hr);
        com_ptr<IMFMediaType> output_type = make_output_RGB32(source_type);
        print(output_type.get());
        if (auto hr = transform->SetOutputType(0, output_type.get(), 0))
            FAIL(hr);
    }
    SECTION("MP4(I420) - RGB32") {
        INFO("IMFMediaSourceEx: MP4(I420) -> RGB32");
        REQUIRE(source_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_I420) == S_OK);
        REQUIRE(source_reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, source_type.get()) ==
                S_OK);
        print(source_type.get());
        if (auto hr = transform->SetInputType(0, source_type.get(), 0))
            FAIL(hr);
        com_ptr<IMFMediaType> output_type = make_output_RGB32(source_type);
        print(output_type.get());
        if (auto hr = transform->SetOutputType(0, output_type.get(), 0))
            FAIL(hr);
    }
    SECTION("MP4(IYUV) - RGB32") {
        INFO("IMFMediaSourceEx: MP4(I420) -> RGB32");
        REQUIRE(source_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_IYUV) == S_OK);
        REQUIRE(source_reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, source_type.get()) ==
                S_OK);
        print(source_type.get());
        if (auto hr = transform->SetInputType(0, source_type.get(), 0))
            FAIL(hr);
        com_ptr<IMFMediaType> output_type = make_output_RGB32(source_type);
        print(output_type.get());
        if (auto hr = transform->SetOutputType(0, output_type.get(), 0))
            FAIL(hr);
    }
    SECTION("MP4(I420) - RGB565") {
        INFO("IMFMediaSourceEx: MP4(I420) -> RGB32");
        REQUIRE(source_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_I420) == S_OK);
        REQUIRE(source_reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, source_type.get()) ==
                S_OK);
        print(source_type.get());
        if (auto hr = transform->SetInputType(0, source_type.get(), 0))
            FAIL(hr);
        com_ptr<IMFMediaType> output_type = make_output_RGB565(source_type);
        print(output_type.get());
        if (auto hr = transform->SetOutputType(0, output_type.get(), 0))
            FAIL(hr);
    }
    print(transform.get(), CLSID_CColorConvertDMO); // requires input/output configuration

    REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL) == S_OK);
    REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL) == S_OK);
    {
        spdlog::warn("testing synchronous read/transform with simplified code");
        DWORD istream = 0, ostream = 0;
        com_ptr<IMFMediaType> output_type{};
        if (auto hr = transform->GetOutputCurrentType(ostream, output_type.put()))
            FAIL(hr);
        size_t count = 0;
        DWORD index{};
        DWORD flags{};
        LONGLONG timestamp{}; // unit 100-nanosecond
        LONGLONG duration{};
        for (com_ptr<IMFSample> input_sample : read_samples(source_reader, //
                                                            index, flags, timestamp, duration)) {
            switch (auto hr = transform->ProcessInput(istream, input_sample.get(), 0)) {
            case S_OK: // MF_E_TRANSFORM_TYPE_NOT_SET, MF_E_NO_SAMPLE_DURATION, MF_E_NO_SAMPLE_TIMESTAMP
                break;
            case MF_E_NOTACCEPTING:
            case MF_E_UNSUPPORTED_D3D_TYPE:
            case E_INVALIDARG:
            default:
                FAIL(hr);
            }
            HRESULT ec = S_OK;
            for (com_ptr<IMFSample> output_sample : decode(transform, ostream, output_type, ec)) {
                if (auto hr = check_sample(output_sample))
                    FAIL(hr);
                ++count;
            }
            switch (ec) {
            case S_OK:
            case MF_E_TRANSFORM_NEED_MORE_INPUT:
                continue;
            default:
                FAIL(ec);
            }
        }
        REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, NULL) == S_OK);
        REQUIRE(transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, NULL) == S_OK);
        REQUIRE(count);
        count = 0;
        // fetch remaining output in the transform
        HRESULT ec = S_OK;
        for (com_ptr<IMFSample> output_sample : decode(transform, ostream, output_type, ec)) {
            if (auto hr = check_sample(output_sample))
                FAIL(hr);
            ++count;
        }
        switch (ec) {
        case S_OK:
        case MF_E_TRANSFORM_NEED_MORE_INPUT:
            break;
        default:
            FAIL(ec);
        }
        REQUIRE(count == 0); // CLSID_CColorConvertDMO won't have leftover
    }
}

HRESULT configure_type_bypass(com_ptr<IMFTransform> transform, com_ptr<IMFMediaType> input_type, //
                              DWORD& istream, DWORD& ostream) {
    spdlog::debug("configure_type_bypass");
    DWORD num_input = 0, num_output = 0;
    if (auto hr = transform->GetStreamCount(&num_input, &num_output); FAILED(hr))
        return hr;
    switch (auto hr = transform->GetStreamIDs(1, &istream, 1, &ostream)) {
    case E_NOTIMPL:
        istream = num_input - 1;
        ostream = num_output - 1;
    case S_OK:
        break;
    default:
        return hr;
    }

    if (auto hr = transform->SetInputType(istream, input_type.get(), 0); FAILED(hr))
        return hr;
    print(input_type.get());

    UINT32 w = 0, h = 0;
    if (auto hr = MFGetAttributeSize(input_type.get(), MF_MT_FRAME_SIZE, &w, &h); FAILED(hr))
        return hr;

    DWORD otype{};
    for (auto candidate : try_output_available_types(transform, ostream, otype)) {
        if (auto hr = MFSetAttributeSize(candidate.get(), MF_MT_FRAME_SIZE, w, h); FAILED(hr))
            return hr;
        spdlog::debug("changinged output candidate frame size");
        print(candidate.get());
        if (auto hr = transform->SetOutputType(ostream, candidate.get(), 0); FAILED(hr))
            return hr;
        return S_OK;
    }
    return E_FAIL; // there was no available type...
}

// see https://docs.microsoft.com/en-us/windows/win32/medfound/videoresizer
// see https://docs.microsoft.com/en-us/windows/win32/medfound/basic-mft-processing-model
TEST_CASE("MFTransform - Video Resizer DSP", "[dsp]") {
    auto on_return = media_startup();

    com_ptr<IMFMediaSourceEx> source{};
    com_ptr<IMFSourceReader> source_reader{};
    com_ptr<IMFMediaType> source_type{};
    make_test_source(source, source_reader, source_type, MFVideoFormat_I420, get_asset_dir() / "fm5p7flyCSY.mp4");

    com_ptr<IMFTransform> transform{};
    REQUIRE(make_transform_video(transform.put(), CLSID_CResizerDMO) == S_OK);
    SECTION("IPropertyStore") {
        com_ptr<IPropertyStore> props{};
        REQUIRE(transform->QueryInterface(props.put()) == S_OK);
        com_ptr<IMediaObject> media_object{};
        REQUIRE(transform->QueryInterface(media_object.put()) == S_OK);
    }
    com_ptr<IWMResizerProps> resizer{};
    SECTION("SetClipRegion(Smaller)") {
        REQUIRE(transform->QueryInterface(resizer.put()) == S_OK);
        REQUIRE(resizer->SetClipRegion(0, 0, 640, 480) == S_OK);
    }
    //SECTION("SetClipRegion(Larger)") {
    //    REQUIRE(transform->QueryInterface(resizer.put()) == S_OK);
    //    REQUIRE(resizer->SetClipRegion(0, 0, 2000, 1000) == S_OK);
    //    // this will return E_FAIL
    //}

    DWORD istream = 0, ostream = 0;
    REQUIRE(configure_type_bypass(transform, source_type, istream, ostream) == S_OK);
    print(transform.get(), CLSID_CResizerDMO);

    REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL) == S_OK);
    REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL) == S_OK);
    {
        spdlog::warn("testing synchronous read/transform with simplified code");
        com_ptr<IMFMediaType> output_type{};
        if (auto hr = transform->GetOutputCurrentType(ostream, output_type.put()))
            FAIL(hr);
        size_t count = 0;
        DWORD index{};
        DWORD flags{};
        LONGLONG timestamp{}; // unit 100-nanosecond
        LONGLONG duration{};
        for (com_ptr<IMFSample> input_sample : read_samples(source_reader, //
                                                            index, flags, timestamp, duration)) {
            switch (auto hr = transform->ProcessInput(istream, input_sample.get(), 0)) {
            case S_OK: // MF_E_TRANSFORM_TYPE_NOT_SET, MF_E_NO_SAMPLE_DURATION, MF_E_NO_SAMPLE_TIMESTAMP
                break;
            case MF_E_NOTACCEPTING:
            case MF_E_UNSUPPORTED_D3D_TYPE:
            case E_INVALIDARG:
            default:
                FAIL(hr);
            }
            HRESULT ec = S_OK;
            for (com_ptr<IMFSample> output_sample : decode(transform, ostream, output_type, ec)) {
                if (auto hr = check_sample(output_sample))
                    FAIL(hr);
                ++count;
            }
            switch (ec) {
            case S_OK:
            case MF_E_TRANSFORM_NEED_MORE_INPUT:
                continue;
            default:
                FAIL(ec);
            }
        }
        REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, NULL) == S_OK);
        REQUIRE(transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, NULL) == S_OK);
        REQUIRE(count);
        count = 0;
        // fetch remaining output in the transform
        HRESULT ec = S_OK;
        for (com_ptr<IMFSample> output_sample : decode(transform, ostream, output_type, ec)) {
            if (auto hr = check_sample(output_sample))
                FAIL(hr);
            ++count;
        }
        switch (ec) {
        case S_OK:
        case MF_E_TRANSFORM_NEED_MORE_INPUT:
            break;
        default:
            FAIL(ec);
        }
        REQUIRE(count == 0); // CLSID_CResizerDMO won't have leftover
    }
}

// see https://docs.microsoft.com/en-us/windows/win32/medfound/video-processor-mft#remarks
// see https://docs.microsoft.com/en-us/windows/win32/medfound/basic-mft-processing-model
TEST_CASE("MFTransform - Video Processor MFT", "[dsp]") {
    auto on_return = media_startup();

    com_ptr<IMFMediaSourceEx> source{};
    com_ptr<IMFSourceReader> source_reader{};
    com_ptr<IMFMediaType> source_type{};
    make_test_source(source, source_reader, source_type, MFVideoFormat_I420, get_asset_dir() / "fm5p7flyCSY.mp4");

    com_ptr<IMFTransform> transform{};
    REQUIRE(make_transform_video(transform.put(), CLSID_VideoProcessorMFT) == S_OK);
    com_ptr<IMFVideoProcessorControl> control{};
    {
        REQUIRE(transform->QueryInterface(control.put()) == S_OK);
        REQUIRE(configure_rectangle(control.get(), source_type.get()) == S_OK);

        // H mirror, corrects the orientation, letterboxes the output as needed
        REQUIRE(control->SetMirror(MF_VIDEO_PROCESSOR_MIRROR::MIRROR_HORIZONTAL) == S_OK);
        REQUIRE(control->SetRotation(MF_VIDEO_PROCESSOR_ROTATION::ROTATION_NORMAL) == S_OK);
        MFARGB color{};
        REQUIRE(control->SetBorderColor(&color) == S_OK);
        // https://docs.microsoft.com/en-us/windows/win32/medfound/media-foundation-work-queue-and-threading-improvements
        com_ptr<IMFRealTimeClientEx> realtime{};
        REQUIRE(transform->QueryInterface(realtime.put()) == S_OK);
    }

    DWORD istream = 0, ostream = 0;
    REQUIRE(configure_type_bypass(transform, source_type, istream, ostream) == S_OK);
    print(transform.get(), CLSID_VideoProcessorMFT);

    REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL) == S_OK);
    REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL) == S_OK);
    {
        spdlog::warn("testing synchronous read/transform with simplified code");
        com_ptr<IMFMediaType> output_type{};
        if (auto hr = transform->GetOutputCurrentType(ostream, output_type.put()))
            FAIL(hr);
        size_t count = 0;
        DWORD index{};
        DWORD flags{};
        LONGLONG timestamp{}; // unit 100-nanosecond
        LONGLONG duration{};
        for (com_ptr<IMFSample> input_sample : read_samples(source_reader, //
                                                            index, flags, timestamp, duration)) {
            switch (auto hr = transform->ProcessInput(istream, input_sample.get(), 0)) {
            case S_OK: // MF_E_TRANSFORM_TYPE_NOT_SET, MF_E_NO_SAMPLE_DURATION, MF_E_NO_SAMPLE_TIMESTAMP
                break;
            case MF_E_NOTACCEPTING:
            case MF_E_UNSUPPORTED_D3D_TYPE:
            case E_INVALIDARG:
            default:
                FAIL(hr);
            }
            HRESULT ec = S_OK;
            for (com_ptr<IMFSample> output_sample : decode(transform, ostream, output_type, ec)) {
                if (auto hr = check_sample(output_sample))
                    FAIL(hr);
                ++count;
            }
            switch (ec) {
            case S_OK:
            case MF_E_TRANSFORM_NEED_MORE_INPUT:
                continue;
            default:
                FAIL(ec);
            }
        }
        REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, NULL) == S_OK);
        REQUIRE(transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, NULL) == S_OK);
        REQUIRE(count);
        count = 0;
        // fetch remaining output in the transform
        HRESULT ec = S_OK;
        for (com_ptr<IMFSample> output_sample : decode(transform, ostream, output_type, ec)) {
            if (auto hr = check_sample(output_sample))
                FAIL(hr);
            ++count;
        }
        switch (ec) {
        case S_OK:
        case MF_E_TRANSFORM_NEED_MORE_INPUT:
            break;
        default:
            FAIL(ec);
        }
        REQUIRE(count == 0); // CLSID_VideoProcessorMFT won't have leftover
    }
}
