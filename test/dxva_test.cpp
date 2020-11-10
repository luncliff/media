/**
 * @file dxva_test.cpp
 * @author github.com/luncliff (luncligg@gmail.com)
 */
#define CATCH_CONFIG_WINDOWS_CRTDBG
#include <catch2/catch.hpp>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.Threading.h>

#include <filesystem>
#include <iostream>
#include <media.hpp>

#include <mmdeviceapi.h>
#include <wincodecsdk.h>
#include <wmsdkidl.h>

#include <comdef.h>
#include <wrl/client.h>
#pragma comment(lib, "comctl32")
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "dxgi.lib")

#include <spdlog/spdlog.h>

using namespace std;
using namespace Microsoft::WRL;

namespace fs = std::filesystem;

fs::path get_asset_dir() noexcept;

void make_media_source(IMFMediaSource** media_source, const fs::path& fpath) {
    REQUIRE(fs::exists(fpath));

    com_ptr<IMFSourceResolver> resolver{};
    REQUIRE(MFCreateSourceResolver(resolver.put()) == S_OK);
    MF_OBJECT_TYPE source_type = MF_OBJECT_INVALID;
    com_ptr<IUnknown> source{};
    REQUIRE(resolver->CreateObjectFromURL(fpath.c_str(),             // URL of the source.
                                          MF_RESOLUTION_MEDIASOURCE, // Create a source object.
                                          NULL,                      // Optional property store.
                                          &source_type,              // Receives the created object type.
                                          source.put()               // Receives a pointer to the media source.
                                          ) == S_OK);
    REQUIRE(source_type == MF_OBJECT_MEDIASOURCE);
    REQUIRE(source->QueryInterface(IID_PPV_ARGS(media_source)) == S_OK);
}

void make_souce_reader(IMFSourceReader** source_reader, IMFMediaSource* media_source) {
    com_ptr<IMFAttributes> reader_attributes{};
    REQUIRE(MFCreateAttributes(reader_attributes.put(), 2) == S_OK);
    REQUIRE(reader_attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                       MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID) == S_OK);
    REQUIRE(reader_attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, 1) == S_OK);
    REQUIRE(MFCreateSourceReaderFromMediaSource(media_source, reader_attributes.get(), source_reader) == S_OK);
}

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/h-264-video-decoder
void make_video_transform(IMFTransform** decoding_transform, IMFSourceReader* source_reader, IMFMediaType** source_type,
                          const IID& decoder_type = CLSID_CMSH264DecoderMFT) {
    // @todo CLSID_CMSMPEGDecoderMFT, CLSID_VideoProcessorMFT, CLSID_CMSH264DecoderMFT, IID_IMFTransform, IID_IUnknown
    com_ptr<IUnknown> transform{};
    REQUIRE(CoCreateInstance(decoder_type, NULL, CLSCTX_INPROC_SERVER, IID_IMFTransform, (void**)transform.put()) ==
            S_OK);
    REQUIRE(transform->QueryInterface(IID_PPV_ARGS(decoding_transform)) == S_OK);

    REQUIRE(source_reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, source_type) == S_OK);
    GUID major{};
    REQUIRE((*source_type)->GetMajorType(&major) == S_OK);
    REQUIRE(major == MFMediaType_Video);
}

void check_device(IMFDXGIDeviceManager* manager, ID3D11Device* expected) {
    HANDLE handle{};
    REQUIRE(manager->OpenDeviceHandle(&handle) == S_OK);
    com_ptr<ID3D11Device> device{};
    REQUIRE(manager->GetVideoService(handle, IID_ID3D11Device, (void**)device.put()) == S_OK);
    REQUIRE(device.get() == expected);
}

void check_device(IMFDXGIDeviceManager* manager, ID3D11VideoDevice* expected) {
    HANDLE handle{};
    REQUIRE(manager->OpenDeviceHandle(&handle) == S_OK);
    com_ptr<ID3D11VideoDevice> device{};
    REQUIRE(manager->GetVideoService(handle, IID_ID3D11VideoDevice, (void**)device.put()) == S_OK);
    REQUIRE(device.get() == expected);
}

void check_device_context(ID3D11Device* device, ID3D11DeviceContext* expected) {
    com_ptr<ID3D11DeviceContext> context{};
    device->GetImmediateContext(context.put());
    REQUIRE(context.get() == expected);
}

void configure_multithread_protection(ID3D11Device* device) {
    com_ptr<ID3D10Multithread> protection{};
    REQUIRE(device->QueryInterface(__uuidof(ID3D10Multithread), (void**)protection.put()) == S_OK);
    CAPTURE(protection->SetMultithreadProtected(TRUE));
}

/// @see source reader scenario in https://docs.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/dxva-video-processing
TEST_CASE("IMFDXGIDeviceManager, ID3D11Device", "[directx]") {
    auto on_return = media_startup();

    // To perform decoding using Direct3D 11, the software decoder must have a pointer to a Direct3D 11 device.
    com_ptr<ID3D11Device> graphics_device{};
    com_ptr<ID3D11DeviceContext> graphics_device_context{};
    {
        D3D_FEATURE_LEVEL level{};
        D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        REQUIRE(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 3,
                                  D3D11_SDK_VERSION, graphics_device.put(), &level,
                                  graphics_device_context.put()) == S_OK);
    }
    configure_multithread_protection(graphics_device.get());

    // The DXGI Device Manager is used to share the Direct3D 11 between components.
    UINT device_manager_token{};
    com_ptr<IMFDXGIDeviceManager> device_manager{};
    REQUIRE(MFCreateDXGIDeviceManager(&device_manager_token, device_manager.put()) == S_OK);

    SECTION("ResetDevice") {
        REQUIRE(device_manager->ResetDevice(graphics_device.get(), device_manager_token) == S_OK);
        check_device(device_manager.get(), graphics_device.get());
        check_device_context(graphics_device.get(), graphics_device_context.get());
    }

    // get video accelerator to ensure D3D11_CREATE_DEVICE_VIDEO_SUPPORT
    SECTION("ID3D11VideoDevice") {
        REQUIRE(device_manager->ResetDevice(graphics_device.get(), device_manager_token) == S_OK);

        com_ptr<ID3D11VideoDevice> video_device{};
        switch (auto hr = graphics_device->QueryInterface(video_device.put())) {
        case S_OK:
            break;
        case E_NOINTERFACE:
        default:
            WARN("failed to query ID3D11VideoDevice.");
            return;
        }
        com_ptr<ID3D11VideoContext> video_context{};
        REQUIRE(graphics_device_context->QueryInterface(video_context.put()) == S_OK);
        check_device(device_manager.get(), video_device.get());
    }
}

/// @brief configure D3D11 if the transform supports it
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/hardware-mfts
HRESULT configure_D3D11_DXGI(IMFTransform* transform, IMFDXGIDeviceManager* device_manager) {
    com_ptr<IMFAttributes> attrs{};
    if (auto hr = transform->GetAttributes(attrs.put())) // return can be E_NOTIMPL
        return hr;

    UINT32 supported{};
    //if (auto hr = attrs->GetUINT32(MF_SA_D3D_AWARE, &supported))
    //    return hr;
    //if (supported == false)
    //    return E_FAIL;
    if (auto hr = attrs->GetUINT32(MF_SA_D3D11_AWARE, &supported))
        return hr;
    if (supported == false)
        return E_FAIL;
    return transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, //
                                     reinterpret_cast<ULONG_PTR>(static_cast<IUnknown*>(device_manager)));
}

HRESULT check_sample(com_ptr<IMFSample> sample);

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/dxva-video-processing
SCENARIO("MFTransform with ID3D11Device", "[directx][!mayfail]") {
    auto on_return = media_startup();

    com_ptr<ID3D11Device> graphics_device{};
    com_ptr<ID3D11DeviceContext> graphics_device_context{};
    {
        D3D_FEATURE_LEVEL level{};
        D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        REQUIRE(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 3,
                                  D3D11_SDK_VERSION, graphics_device.put(), &level,
                                  graphics_device_context.put()) == S_OK);
    }
    configure_multithread_protection(graphics_device.get());

    // The DXGI Device Manager is used to share the Direct3D 11 between components.
    UINT device_manager_token{};
    com_ptr<IMFDXGIDeviceManager> device_manager{};
    REQUIRE(MFCreateDXGIDeviceManager(&device_manager_token, device_manager.put()) == S_OK);
    REQUIRE(device_manager->ResetDevice(graphics_device.get(), device_manager_token) == S_OK);

    GIVEN("CLSID_CMSH264DecoderMFT") {
        com_ptr<IMFMediaSourceEx> source{};
        MF_OBJECT_TYPE media_object_type = MF_OBJECT_INVALID;
        REQUIRE(resolve(get_asset_dir() / "fm5p7flyCSY.mp4", source.put(), media_object_type) == S_OK);

        com_ptr<IMFSourceReader> source_reader{};
        REQUIRE(MFCreateSourceReaderFromMediaSource(source.get(), nullptr, source_reader.put()) == S_OK);

        com_ptr<IMFMediaType> input_type{};
        REQUIRE(source_reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, input_type.put()) ==
                S_OK);

        com_ptr<IMFTransform> transform{};
        REQUIRE(make_transform_H264(transform.put()) == S_OK);

        DWORD num_input = 0, num_output = 0;
        DWORD istream_id = 0, ostream_id = 0;
        GUID in_subtype{}, out_subtype{};
        {
            REQUIRE(transform->GetStreamCount(&num_input, &num_output) == S_OK);
            REQUIRE(transform->GetStreamIDs(1, &istream_id, 1, &ostream_id) != MF_E_BUFFERTOOSMALL);

            if (auto hr = transform->SetInputType(istream_id, input_type.get(), 0))
                FAIL(hr);
            if (auto hr = input_type->GetGUID(MF_MT_SUBTYPE, &in_subtype))
                FAIL(hr);
            DWORD type_index = 0;
            for (com_ptr<IMFMediaType> output_type : try_output_available_types(transform, ostream_id, type_index)) {
                if (auto hr = transform->SetOutputType(ostream_id, output_type.get(), 0))
                    FAIL(hr);
                if (auto hr = output_type->GetGUID(MF_MT_SUBTYPE, &out_subtype))
                    FAIL(hr);
                break;
            }
            spdlog::debug("- transform_configuration:");
            spdlog::debug("  input: {}", to_readable(in_subtype));
            spdlog::debug("  output: {}", to_readable(out_subtype));
        }

        MFT_INPUT_STREAM_INFO input_stream_info{};
        if (auto hr = transform->GetInputStreamInfo(istream_id, &input_stream_info))
            FAIL(hr);
        MFT_OUTPUT_STREAM_INFO output_stream_info{};
        if (auto hr = transform->GetOutputStreamInfo(ostream_id, &output_stream_info))
            FAIL(hr);

        WHEN("Synchronous Transform") {
            REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL) == S_OK);
            REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL) == S_OK);

            com_ptr<IMFMediaType> output_type{};
            if (auto hr = transform->GetOutputCurrentType(ostream_id, output_type.put()))
                FAIL(hr);

            size_t count = 0;
            DWORD index{};
            DWORD flags{};
            LONGLONG timestamp{}; // unit 100-nanosecond
            LONGLONG duration{};
            for (com_ptr<IMFSample> input_sample : read_samples(source_reader, //
                                                                index, flags, timestamp, duration)) {
                input_sample->SetSampleTime(timestamp);
                switch (auto hr = transform->ProcessInput(istream_id, input_sample.get(), 0)) {
                case S_OK: // MF_E_TRANSFORM_TYPE_NOT_SET, MF_E_NO_SAMPLE_DURATION, MF_E_NO_SAMPLE_TIMESTAMP
                    break;
                case MF_E_UNSUPPORTED_D3D_TYPE:
                case MF_E_NOTACCEPTING:
                default:
                    FAIL(hr);
                }
                for (com_ptr<IMFSample> output_sample : decode(transform, output_type, ostream_id)) {
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
            for (com_ptr<IMFSample> output_sample : decode(transform, output_type, ostream_id)) {
                if (auto hr = check_sample(output_sample))
                    FAIL(hr);
                ++count;
            }
            REQUIRE(count);
        }
    }
    GIVEN("CLSID_VideoProcessorMFT") {
        com_ptr<IMFTransform> transform{};
        REQUIRE(make_transform_video(transform.put(), CLSID_VideoProcessorMFT) == S_OK);
        {
            com_ptr<IMFAttributes> attrs{};
            REQUIRE(transform->GetAttributes(attrs.put()) == S_OK);
            UINT32 supported = FALSE;
            REQUIRE(attrs->GetUINT32(MF_SA_D3D11_AWARE, &supported) == S_OK);
            REQUIRE(supported);
            REQUIRE(transform->ProcessMessage(
                        MFT_MESSAGE_SET_D3D_MANAGER, //
                        reinterpret_cast<ULONG_PTR>(static_cast<IUnknown*>(device_manager.get()))) == S_OK);
        }
        {
            com_ptr<IMFVideoProcessorControl> control{};
            REQUIRE(transform->QueryInterface(control.put()) == S_OK);
            REQUIRE(control->SetMirror(MIRROR_NONE) == S_OK);
            REQUIRE(control->SetRotation(ROTATION_NONE) == S_OK);
            RECT region{0, 0, 1280, 720};
            REQUIRE(control->SetDestinationRectangle(&region) == S_OK);
        }
        print(transform.get());

        com_ptr<IMFMediaSourceEx> source_h264{};
        com_ptr<IMFSourceReader> reader_h264{};
        com_ptr<IMFTransform> transform_h264{};
        com_ptr<IMFMediaType> output_h264{};
        {
            MF_OBJECT_TYPE media_object_type = MF_OBJECT_INVALID;
            REQUIRE(resolve(get_asset_dir() / "fm5p7flyCSY.mp4", source_h264.put(), media_object_type) == S_OK);
            REQUIRE(MFCreateSourceReaderFromMediaSource(source_h264.get(), nullptr, reader_h264.put()) == S_OK);
            com_ptr<IMFMediaType> input_h264{};
            REQUIRE(reader_h264->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, input_h264.put()) ==
                    S_OK);
            REQUIRE(make_transform_H264(transform_h264.put()) == S_OK);
            REQUIRE(transform_h264->SetInputType(0, input_h264.get(), 0) == S_OK);
            REQUIRE(try_output_type(transform_h264, 0, MFVideoFormat_NV12, output_h264.put()) == S_OK);
        }

        DWORD num_input = 0, num_output = 0;
        REQUIRE(transform->GetStreamCount(&num_input, &num_output) == S_OK);
        REQUIRE(num_input == 1);
        REQUIRE(num_output == 1);
        const DWORD istream = 0, ostream = 0;
        //// E_NOTIMPL is expected. we can start from id '0'
        //CAPTURE(transform->GetStreamIDs(num_input, istreams.get(), num_output, ostreams.get()));

        WHEN("Synchronous Transform") {
            com_ptr<IMFMediaType> input_type = output_h264;

            print(input_type.get());
            if (auto hr = transform->SetInputType(istream, input_type.get(), 0))
                FAIL(hr);

            com_ptr<IMFMediaType> output_type{}; // MFVideoFormat_RGB32
            DWORD type_index = 0;
            for (auto candidate : try_output_available_types(transform, ostream, type_index)) {
                if (auto hr = transform->SetOutputType(ostream, candidate.get(), MFT_SET_TYPE_TEST_ONLY))
                    continue;
                output_type = candidate;
                break;
            }
            REQUIRE(output_type);
            if (auto hr = transform->SetOutputType(ostream, output_type.get(), 0))
                FAIL(hr);
            print(output_type.get());

            REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL) == S_OK);
            REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL) == S_OK);

            HRESULT ec = S_OK;
            size_t count = 0;
            for (com_ptr<IMFSample> input_sample : process(transform_h264, 0, 0, reader_h264, ec)) {
                switch (auto hr = transform->ProcessInput(istream, input_sample.get(), 0)) {
                case S_OK: // MF_E_TRANSFORM_TYPE_NOT_SET, MF_E_NO_SAMPLE_DURATION, MF_E_NO_SAMPLE_TIMESTAMP
                    break;
                case MF_E_UNSUPPORTED_D3D_TYPE:
                case MF_E_NOTACCEPTING:
                default:
                    FAIL(hr);
                }
                for (com_ptr<IMFSample> output_sample : decode(transform, output_type, ostream)) {
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
            for (com_ptr<IMFSample> output_sample : decode(transform, output_type, ostream)) {
                if (auto hr = check_sample(output_sample))
                    FAIL(hr);
                ++count;
            }
            REQUIRE(count);
        }
    }
}

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/direct3d-aware-mfts
/// @see https://github.com/microsoft/Windows-classic-samples/tree/master/Samples/DX11VideoRenderer
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/basic-mft-processing-model#process-data
TEST_CASE("MFTransform with Direct11 Device", "[directx][!mayfail]") {
    auto on_exit = media_startup();

    // To perform decoding using Direct3D 11, the software decoder must have a pointer to a Direct3D 11 device.
    com_ptr<ID3D11Device> graphics_device{};
    com_ptr<ID3D11DeviceContext> graphics_device_context{};
    {
        D3D_FEATURE_LEVEL level{};
        D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        REQUIRE(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 3,
                                  D3D11_SDK_VERSION, graphics_device.put(), &level,
                                  graphics_device_context.put()) == S_OK);
    }
    // multi-thread protection
    configure_multithread_protection(graphics_device.get());

    // get video accelerator to ensure D3D11_CREATE_DEVICE_VIDEO_SUPPORT
    com_ptr<ID3D11VideoDevice> video_device{};
    REQUIRE(graphics_device->QueryInterface(__uuidof(ID3D11VideoDevice), //
                                            (void**)video_device.put()) == S_OK);
    com_ptr<ID3D11VideoContext> video_context{};
    REQUIRE(graphics_device_context->QueryInterface(__uuidof(ID3D11VideoContext), //
                                                    (void**)video_context.put()) == S_OK);

    // The DXGI Device Manager is used to share the Direct3D 11 between components.
    UINT device_manager_token{};
    com_ptr<IMFDXGIDeviceManager> device_manager{};
    REQUIRE(MFCreateDXGIDeviceManager(&device_manager_token, device_manager.put()) == S_OK);
    REQUIRE(device_manager->ResetDevice(graphics_device.get(), device_manager_token) == S_OK);

    check_device(device_manager.get(), graphics_device.get());
    check_device(device_manager.get(), video_device.get());
    check_device_context(graphics_device.get(), graphics_device_context.get());

    SECTION("IMFSourceReader/IMFTransform") {
        com_ptr<IMFMediaSource> media_source{};
        make_media_source(media_source.put(), fs::absolute(get_asset_dir() / "fm5p7flyCSY.mp4"));
        com_ptr<IMFSourceReader> source_reader{};
        make_souce_reader(source_reader.put(), media_source.get());

        com_ptr<IMFTransform> transform{};
        com_ptr<IMFMediaType> source_type{};
        make_video_transform(transform.put(), source_reader.get(), source_type.put());

        // configure Direct3D 11
        com_ptr<IMFAttributes> transform_attributes{};
        REQUIRE(transform->GetAttributes(transform_attributes.put()) == S_OK);
        REQUIRE(transform_attributes->SetUINT32(MF_SA_D3D_AWARE, TRUE) == S_OK);
        REQUIRE(transform_attributes->SetUINT32(MF_SA_D3D11_AWARE, TRUE) == S_OK);
        REQUIRE(transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, //
                                          reinterpret_cast<ULONG_PTR>(static_cast<IUnknown*>(device_manager.get()))) ==
                S_OK);

        SECTION("ProcessMessage") {
            com_ptr<IMFMediaType> input_media_type{};
            REQUIRE(MFCreateMediaType(input_media_type.put()) == S_OK);
            REQUIRE(source_type->CopyAllItems(input_media_type.get()) == S_OK);
            if (auto hr = transform->SetInputType(0, input_media_type.get(), 0))
                FAIL(hr);

            com_ptr<IMFMediaType> output_media_type{};
            REQUIRE(MFCreateMediaType(output_media_type.put()) == S_OK);
            REQUIRE(source_type->CopyAllItems(output_media_type.get()) == S_OK);
            //  MFVideoFormat_NV12, MFVideoFormat_YV12 MFVideoFormat_IYUV MFVideoFormat_I420 MFVideoFormat_YUY2
            if (auto hr = output_media_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_I420))
                FAIL(hr);

            REQUIRE(transform->SetOutputType(0, output_media_type.get(), 0) == S_OK);

            // configure leftovers
            REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL) == S_OK);
            REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL) == S_OK);
            REQUIRE(transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL) == S_OK);

            MFT_OUTPUT_STREAM_INFO output_stream_info{};
            output_stream_info.dwFlags = MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;
            REQUIRE(transform->GetOutputStreamInfo(0, &output_stream_info) == S_OK);
            com_ptr<IMFAttributes> output_stream_attributes{};
            REQUIRE(transform->GetOutputStreamAttributes(0, output_stream_attributes.put()) == S_OK);

            /// @see https://docs.microsoft.com/en-us/windows/win32/api/mfapi/nf-mfapi-mfcreatedxsurfacebuffer
            SECTION("Allocating Uncompressed Buffers") {
                //D3D11_VIDEO_PROCESSOR_CAPS caps{};
                D3D11_SUBRESOURCE_DATA subdata{};
                D3D11_TEXTURE2D_DESC desc{};
                com_ptr<ID3D11Texture2D> texture{};
                REQUIRE(graphics_device->CreateTexture2D(&desc, &subdata, texture.put()) == S_OK);

                com_ptr<IMFMediaBuffer> surface_buffer{};
                REQUIRE(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture.get(), 1, FALSE,
                                                  surface_buffer.put()) == S_OK);

                com_ptr<IMFSample> sample{};
                //REQUIRE(MFCreateVideoSampleFromSurface(nullptr, sample.put()) == S_OK);
                REQUIRE(MFCreateSample(sample.put()) == S_OK);
                REQUIRE(sample->AddBuffer(surface_buffer.get()) == S_OK);

                D3D11_VIDEO_DECODER_DESC decoder_desc{};
                D3D11_VIDEO_DECODER_CONFIG decoder_config{};
                com_ptr<ID3D11VideoDecoder> decoder{};
                REQUIRE(video_device->CreateVideoDecoder(&decoder_desc, &decoder_config, decoder.put()) == S_OK);
            }
        }

        // then configure input/output
        SECTION("check available input/output config") {
            DWORD num_input = 0, num_output = 0;
            REQUIRE(transform->GetStreamCount(&num_input, &num_output) == S_OK);
            REQUIRE_FALSE(num_input == 0);
            REQUIRE_FALSE(num_output == 0);

            DWORD istream_id = 0, ostream_id = 0;
            switch (auto hr = transform->GetStreamIDs(num_input, &istream_id, num_output, &ostream_id)) {
            case E_NOTIMPL: // this is expected. we can start from id '0'
            case S_OK:
                break;
            case MF_E_BUFFERTOOSMALL:
            default:
                FAIL(hr);
            }

            HRESULT ec = S_OK;
            spdlog::info("- available_transform:");

            for (com_ptr<IMFMediaType> output_type : get_output_available_types(transform, num_output, ec)) {
                if (auto hr = transform->SetOutputType(ostream_id, output_type.get(), 0))
                    FAIL(hr);
                GUID out_subtype{};
                if (auto hr = output_type->GetGUID(MF_MT_SUBTYPE, &out_subtype))
                    FAIL(hr);
                DWORD type_index = 0;
                for (com_ptr<IMFMediaType> input_type : try_input_available_types(transform, ostream_id, type_index)) {
                    GUID in_subtype{};
                    if (auto hr = input_type->GetGUID(MF_MT_SUBTYPE, &in_subtype))
                        FAIL(hr);
                    spdlog::info("  - output: {}", to_readable(out_subtype));
                    spdlog::info("    input: {}", to_readable(in_subtype));
                }
            }
            REQUIRE(ec == MF_E_TRANSFORM_TYPE_NOT_SET);

            for (com_ptr<IMFMediaType> input_type : get_input_available_types(transform, num_input, ec)) {
                if (auto hr = transform->SetInputType(istream_id, input_type.get(), 0))
                    FAIL(hr);
                GUID in_subtype{};
                if (auto hr = input_type->GetGUID(MF_MT_SUBTYPE, &in_subtype))
                    FAIL(hr);
                DWORD type_index = 0;
                for (com_ptr<IMFMediaType> output_type :
                     try_output_available_types(transform, ostream_id, type_index)) {
                    GUID out_subtype{};
                    if (auto hr = output_type->GetGUID(MF_MT_SUBTYPE, &out_subtype))
                        FAIL(hr);

                    spdlog::info("  - input: {}", to_readable(in_subtype));
                    spdlog::info("    output: {}", to_readable(out_subtype));
                }
            }
            REQUIRE(ec == MF_E_NO_MORE_TYPES);
        }
    }
}
