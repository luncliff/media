/**
 * @file dxva_test.cpp
 * @author github.com/luncliff (luncliff@gmail.com)
 * @see https://docs.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation
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

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_2.h>
#include <dxgi1_2.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "dxgi.lib")
#include <d3d9.h>
#include <dxva2api.h>

#include <spdlog/spdlog.h>

using namespace std;

namespace fs = std::filesystem;

fs::path get_asset_dir() noexcept;

HRESULT configure_type_bypass(com_ptr<IMFTransform> transform, com_ptr<IMFMediaType> input_type, //
                              DWORD& istream, DWORD& ostream);
HRESULT consume(com_ptr<IMFSourceReader> source_reader, com_ptr<IMFTransform> transform, DWORD istream, DWORD ostream);

HRESULT print_formats(gsl::not_null<ID3D11Device*> device, DXGI_FORMAT format) {
    UINT flags = 0;
    if (auto hr = device->CheckFormatSupport(format, &flags)) {
        spdlog::error("{} not supported", format);
        return hr;
    }
    spdlog::info("  - DXGI_FORMAT: {:#08x}", format);
    auto supports = [flags](D3D11_FORMAT_SUPPORT mask) -> bool { return flags & mask; };
    spdlog::info("    D3D11_FORMAT_SUPPORT_TEXTURE2D: {}", supports(D3D11_FORMAT_SUPPORT_TEXTURE2D));
    spdlog::info("    D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT: {}",
                 supports(D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT));
    spdlog::info("    D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT: {}",
                 supports(D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT));
    spdlog::info("    D3D11_FORMAT_SUPPORT_VIDEO_ENCODER: {}", supports(D3D11_FORMAT_SUPPORT_VIDEO_ENCODER));
    spdlog::info("    D3D11_FORMAT_SUPPORT_DECODER_OUTPUT: {}", supports(D3D11_FORMAT_SUPPORT_DECODER_OUTPUT));
    return S_OK;
}

TEST_CASE("ID3D11Device", "[!mayfail]") {
    com_ptr<ID3D11Device> device{};
    com_ptr<ID3D11DeviceContext> context{};

    SECTION("11 without level") {
        D3D_FEATURE_LEVEL level{};
        REQUIRE(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL,
                                  D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, NULL, 0,
                                  D3D11_SDK_VERSION, device.put(), &level, context.put()) == S_OK);
        REQUIRE(device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_10_1);

        // https://docs.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-legacy-formats
        // https://docs.microsoft.com/en-us/windows/win32/direct2d/supported-pixel-formats-and-alpha-modes
        // todo: https://docs.microsoft.com/en-us/windows/win32/medfound/video-subtype-guids#uncompressed-rgb-formats
        spdlog::info("ID3D11Device:");
        print_formats(device.get(), DXGI_FORMAT_B8G8R8X8_UNORM); // == D3DFMT_X8R8G8B8
        print_formats(device.get(), DXGI_FORMAT_B8G8R8A8_UNORM); // == D3DFMT_A8R8G8B8
        print_formats(device.get(), DXGI_FORMAT_R8G8B8A8_UNORM);

        {
            D3D11_FEATURE_DATA_THREADING features{};
            REQUIRE(device->CheckFeatureSupport(D3D11_FEATURE_THREADING, &features,
                                                sizeof(D3D11_FEATURE_DATA_THREADING)) == S_OK);
        }
    }

    // https://docs.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-d3d11createdevice
    SECTION("11.1") {
        D3D_FEATURE_LEVEL level{};
        D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1};
        REQUIRE(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL,
                                  D3D11_CREATE_DEVICE_DEBUG | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 1,
                                  D3D11_SDK_VERSION, device.put(), &level, context.put()) == S_OK);
        REQUIRE(device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1);

        com_ptr<ID3D11Device1> device1{};
        REQUIRE(device->QueryInterface(device1.put()) == S_OK);
        {
            D3D11_FEATURE_DATA_D3D9_OPTIONS features{};
            REQUIRE(device->CheckFeatureSupport(D3D11_FEATURE_D3D9_OPTIONS, &features,
                                                sizeof(D3D11_FEATURE_DATA_D3D9_OPTIONS)) == S_OK);
        }
        {
            D3D11_FEATURE_DATA_D3D11_OPTIONS features{};
            REQUIRE(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &features,
                                                sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS)) == S_OK);
        }
    }

    // https://docs.microsoft.com/en-us/windows/win32/api/d3d11_2/nn-d3d11_2-id3d11device2
    SECTION("11.2") {
        D3D_FEATURE_LEVEL level{};
        D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1};
        REQUIRE(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL,
                                  D3D11_CREATE_DEVICE_DEBUG | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 1,
                                  D3D11_SDK_VERSION, device.put(), &level, context.put()) == S_OK);
        REQUIRE(device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1);

        com_ptr<ID3D11Device2> device2{};
        REQUIRE(device->QueryInterface(device2.put()) == S_OK);
        {
            D3D11_FEATURE_DATA_D3D9_OPTIONS1 features{};
            REQUIRE(device->CheckFeatureSupport(D3D11_FEATURE_D3D9_OPTIONS1, &features,
                                                sizeof(D3D11_FEATURE_DATA_D3D9_OPTIONS1)) == S_OK);
        }
        {
            D3D11_FEATURE_DATA_D3D11_OPTIONS1 features{};
            REQUIRE(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &features,
                                                sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS1)) == S_OK);
        }
        {
            D3D11_FEATURE_DATA_D3D9_SIMPLE_INSTANCING_SUPPORT features{};
            REQUIRE(device->CheckFeatureSupport(D3D11_FEATURE_D3D9_SIMPLE_INSTANCING_SUPPORT, &features,
                                                sizeof(D3D11_FEATURE_DATA_D3D9_SIMPLE_INSTANCING_SUPPORT)) == S_OK);
        }
    }
}

SCENARIO("ID3D11Texture2D as IDXGISurface", "[!mayfail]") {
    auto on_return = media_startup();

    com_ptr<ID3D11Device> device{};
    com_ptr<ID3D11DeviceContext> context{};
    {
        // todo: add D3D11_CREATE_DEVICE_BGRA_SUPPORT?
        D3D_FEATURE_LEVEL level{};
        D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1};
        REQUIRE(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL,
                                  D3D11_CREATE_DEVICE_DEBUG | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 1,
                                  D3D11_SDK_VERSION, device.put(), &level, context.put()) == S_OK);
    }
    D3D11_FEATURE_DATA_D3D11_OPTIONS features{};
    REQUIRE(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &features,
                                        sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS)) == S_OK);

    // see https://docs.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_usage
    // see https://docs.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_bind_flag
    com_ptr<ID3D11Texture2D> texture2d{};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 600;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    //desc.BindFlags |= D3D11_BIND_DECODER; // requires D3D_FEATURE_LEVEL_11_1
    //desc.BindFlags |= D3D11_BIND_VIDEO_ENCODER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.SampleDesc.Quality = 0;
    desc.SampleDesc.Count = 1;
    REQUIRE(device->CreateTexture2D(&desc, nullptr, texture2d.put()) == S_OK);

    com_ptr<IDXGISurface> surface{};
    REQUIRE(texture2d->QueryInterface(surface.put()) == S_OK);

    com_ptr<IMFMediaBuffer> media_buffer{};
    UINT index = 0;
    REQUIRE(MFCreateDXGISurfaceBuffer(IID_ID3D11Texture2D, surface.get(), index, TRUE, media_buffer.put()) == S_OK);

    com_ptr<IMFDXGIBuffer> dxgi_buffer{};
    REQUIRE(media_buffer->QueryInterface(dxgi_buffer.put()) == S_OK);

    SECTION("SubresourceIndex") {
        REQUIRE(dxgi_buffer->GetSubresourceIndex(&index) == S_OK);
        com_ptr<ID3D11Texture2D> resource{};
        REQUIRE(dxgi_buffer->GetResource(IID_PPV_ARGS(resource.put())) == S_OK);
        REQUIRE(texture2d == resource);
    }
}

void check_device(IMFDXGIDeviceManager* manager, ID3D11Device* expected) {
    HANDLE handle{};
    REQUIRE(manager->OpenDeviceHandle(&handle) == S_OK);
    auto on_return = gsl::finally([manager, handle]() { manager->CloseDeviceHandle(handle); });

    com_ptr<ID3D11Device> device{};
    REQUIRE(manager->GetVideoService(handle, IID_PPV_ARGS(device.put())) == S_OK);
    REQUIRE(device.get() == expected);
}

void check_device(IMFDXGIDeviceManager* manager, ID3D11VideoDevice* expected) {
    HANDLE handle{};
    REQUIRE(manager->OpenDeviceHandle(&handle) == S_OK);
    auto on_return = gsl::finally([manager, handle]() { manager->CloseDeviceHandle(handle); });

    com_ptr<ID3D11VideoDevice> device{};
    REQUIRE(manager->GetVideoService(handle, IID_PPV_ARGS(device.put())) == S_OK);
    REQUIRE(device.get() == expected);
}

void check_device_context(ID3D11Device* device, ID3D11DeviceContext* expected) {
    com_ptr<ID3D11DeviceContext> context{};
    device->GetImmediateContext(context.put());
    REQUIRE(context.get() == expected);
}

void configure_multithread_protection(ID3D11Device* device) {
    com_ptr<ID3D10Multithread> protection{};
    REQUIRE(device->QueryInterface(protection.put()) == S_OK);
    CAPTURE(protection->SetMultithreadProtected(TRUE));
}

TEST_CASE("IMFDXGIDeviceManager - ResetDevice", "[!mayfail]") {
    auto on_return = media_startup();

    com_ptr<ID3D11Device> device{};
    com_ptr<ID3D11DeviceContext> context{};
    D3D_FEATURE_LEVEL level{};
    REQUIRE(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, NULL, 0,
                              D3D11_SDK_VERSION, device.put(), &level, context.put()) == S_OK);

    // The DXGI Device Manager is used to share the Direct3D 11 between components.
    UINT cookie{};
    com_ptr<IMFDXGIDeviceManager> device_manager{};
    REQUIRE(MFCreateDXGIDeviceManager(&cookie, device_manager.put()) == S_OK);

    REQUIRE(device_manager->ResetDevice(device.get(), cookie) == S_OK);
    check_device(device_manager.get(), device.get());
    check_device_context(device.get(), context.get());
}

/// @see source reader scenario in https://docs.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/dxva-video-processing
TEST_CASE("ID3D11VideoDevice", "[dxva][!mayfail]") {
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

    REQUIRE(device_manager->ResetDevice(graphics_device.get(), device_manager_token) == S_OK);
    com_ptr<ID3D11VideoDevice> video_device{};
    REQUIRE(graphics_device->QueryInterface(video_device.put()) == S_OK);
    com_ptr<ID3D11VideoContext> video_context{};
    REQUIRE(graphics_device_context->QueryInterface(video_context.put()) == S_OK);

    // get video accelerator to ensure D3D11_CREATE_DEVICE_VIDEO_SUPPORT
    // see https://docs.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation
    // see https://docs.microsoft.com/en-us/windows/uwp/gaming/feature-mapping
    SECTION("Video Decoder") {

        DWORD num_profile = video_device->GetVideoDecoderProfileCount();
        REQUIRE(num_profile > 0);
        for (auto i = 0u; i < num_profile; ++i) {
            D3D11_VIDEO_DECODER_DESC desc{};
            if (auto hr = video_device->GetVideoDecoderProfile(i, &desc.Guid))
                FAIL(hr);

            desc.SampleWidth = desc.SampleHeight = 600;
            desc.OutputFormat = DXGI_FORMAT_NV12;
            BOOL supported = FALSE;
            if (auto hr = video_device->CheckVideoDecoderFormat(&desc.Guid, desc.OutputFormat, &supported))
                FAIL(hr);
            if (supported == false) {
                spdlog::error("Video decoder doesn't support: {}", desc.OutputFormat);
                spdlog::debug(" - D3D11_VIDEO_DECODER_DESC");
                spdlog::debug("   guid: {}", to_readable(desc.Guid));
                continue;
            }

            UINT num_config = 0;
            if (auto hr = video_device->GetVideoDecoderConfigCount(&desc, &num_config))
                FAIL(hr);

            while (num_config--) {
                D3D11_VIDEO_DECODER_CONFIG config{};
                if (auto hr = video_device->GetVideoDecoderConfig(&desc, num_config, &config))
                    FAIL(hr);
                com_ptr<ID3D11VideoDecoder> video_decoder{};
                if (auto hr = video_device->CreateVideoDecoder(&desc, &config, video_decoder.put())) {
                    spdlog::error("video_device->CreateVideoDecoder: {:#08x}", hr);
                    spdlog::debug(" - D3D11_VIDEO_DECODER_DESC");
                    spdlog::debug("   guid: {}", to_readable(desc.Guid));
                    spdlog::debug("   format: {}", desc.OutputFormat);
                }
            }
        }
    }
    // https://docs.microsoft.com/en-us/windows/win32/medfound/direct3d-11-video-apis
    SECTION("ID3D11VideoProcessor") {
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};

        com_ptr<ID3D11VideoProcessorEnumerator> video_processor_enumerator{};
        if (auto hr = video_device->CreateVideoProcessorEnumerator(&desc, video_processor_enumerator.put()))
            FAIL(hr);
        // REQUIRE(video_device->CreateVideoProcessorEnumerator(nullptr, video_processor_enumerator.put()) == S_OK);

        com_ptr<ID3D11VideoProcessor> video_processor{};
        REQUIRE(video_device->CreateVideoProcessor(video_processor_enumerator.get(), 0, video_processor.put()) == S_OK);
    }
}

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/dxva-video-processing
SCENARIO("MFTransform with ID3D11Device", "[dxva][!mayfail]") {
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

    com_ptr<IMFMediaType> input_type{};
    REQUIRE(make_video_output_RGB32(input_type.put()) == S_OK);
    REQUIRE(MFSetAttributeSize(input_type.get(), MF_MT_FRAME_SIZE, 1280, 720) == S_OK);

    GIVEN("CLSID_VideoProcessorMFT") {
        com_ptr<IMFTransform> transform{};
        REQUIRE(make_transform_video(transform.put(), CLSID_VideoProcessorMFT) == S_OK);
        CAPTURE(configure_D3D11_DXGI(transform.get(), device_manager.get()));

        com_ptr<IMFVideoProcessorControl> control{};
        REQUIRE(transform->QueryInterface(control.put()) == S_OK);
        REQUIRE(configure_rectangle(control.get(), input_type.get()) == S_OK);
        //REQUIRE(control->SetMirror(MIRROR_VERTICAL) == S_OK);
        //REQUIRE(control->SetRotation(ROTATION_NONE) == S_OK);

        // with configure_D3D11_DXGI, some formats might not be available (like I420)
        const DWORD istream = 0, ostream = 0;
        REQUIRE(transform->SetInputType(istream, input_type.get(), 0) == S_OK);

        DWORD type_index{};
        for (auto candidate : try_output_available_types(transform, ostream, type_index)) {
            if (auto hr = MFSetAttributeSize(candidate.get(), MF_MT_FRAME_SIZE, 1280, 720); FAILED(hr))
                FAIL(hr);
            REQUIRE(transform->SetOutputType(ostream, candidate.get(), 0) == S_OK);
            return;
        }
        FAIL("there must be at least 1 type selected");
    }

    GIVEN("CLSID_CColorConvertDMO")
    THEN("E_NOTIMPL") {
        com_ptr<IMFTransform> transform{};
        REQUIRE(make_transform_video(transform.put(), CLSID_CColorConvertDMO) == S_OK);
        REQUIRE(configure_D3D11_DXGI(transform.get(), device_manager.get()) == E_NOTIMPL);
    }
}
