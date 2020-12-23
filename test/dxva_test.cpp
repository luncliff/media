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

/// @see source reader scenario in https://docs.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/dxva-video-processing
TEST_CASE("IMFDXGIDeviceManager, ID3D11Device", "[dxva][!mayfail]") {
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
    // see https://docs.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation
    // see https://docs.microsoft.com/en-us/windows/uwp/gaming/feature-mapping
    SECTION("Direct3D 11 Video Decoding") {
        REQUIRE(device_manager->ResetDevice(graphics_device.get(), device_manager_token) == S_OK);
        com_ptr<ID3D11VideoDevice> video_device{};
        REQUIRE(graphics_device->QueryInterface(video_device.put()) == S_OK);
        com_ptr<ID3D11VideoContext> video_context{};
        REQUIRE(graphics_device_context->QueryInterface(video_context.put()) == S_OK);

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
        com_ptr<ID3D11VideoProcessor> video_processor{};
        REQUIRE(video_device->CreateVideoProcessor(nullptr, 0, video_processor.put()) == S_OK);
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
