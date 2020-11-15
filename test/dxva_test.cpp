/**
 * @file dxva_test.cpp
 * @author github.com/luncliff (luncliff@gmail.com)
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

void make_test_source(com_ptr<IMFMediaSourceEx>& source, com_ptr<IMFSourceReader>& source_reader,
                      com_ptr<IMFMediaType>& source_type, //
                      const GUID& output_subtype, const fs::path& fpath);
HRESULT check_sample(com_ptr<IMFSample> sample);

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
TEST_CASE("IMFDXGIDeviceManager, ID3D11Device", "[dxva]") {
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
        if (num_profile) {
            spdlog::info("- video_decoder_profile:");
            for (auto i = 0u; i < num_profile; ++i) {
                GUID profile{};
                video_device->GetVideoDecoderProfile(i, &profile);
                spdlog::info("  - guid: {}", to_readable(profile));
                BOOL supported = FALSE;
                if (auto hr = video_device->CheckVideoDecoderFormat(&profile, DXGI_FORMAT_R8G8B8A8_UNORM, &supported))
                    FAIL(hr);
                if (supported)
                    spdlog::debug("    - DXGI_FORMAT_R8G8B8A8_UNORM");
            }
        }
        D3D11_VIDEO_DECODER_DESC config{};
        // video_device->GetVideoDecoderConfigCount()
        //com_ptr<ID3D11VideoDecoder> video_decoder{};
        //REQUIRE(video_device->CreateVideoDecoder(nullptr, nullptr, video_decoder.put()) == S_OK);
        //com_ptr<ID3D11VideoProcessor> video_processor{};
        //REQUIRE(video_device->CreateVideoProcessor(nullptr, 0, video_processor.put()) == S_OK);
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

    com_ptr<IMFMediaSourceEx> source{};
    com_ptr<IMFSourceReader> source_reader{};
    com_ptr<IMFMediaType> source_type{};
    make_test_source(source, source_reader, source_type, MFVideoFormat_H264, get_asset_dir() / "fm5p7flyCSY.mp4");

    GIVEN("CLSID_CMSH264DecoderMFT") {
        com_ptr<IMFTransform> transform{};
        REQUIRE(make_transform_video(transform.put(), CLSID_CMSH264DecoderMFT) == S_OK);
        REQUIRE(configure_acceleration_H264(transform.get()) == S_OK);
        CAPTURE(configure_D3D11_DXGI(transform.get(), device_manager.get()));

        print(transform.get(), CLSID_CMSH264DecoderMFT);
    }
    GIVEN("CLSID_VideoProcessorMFT") {
        com_ptr<IMFTransform> transform{};
        REQUIRE(make_transform_video(transform.put(), CLSID_VideoProcessorMFT) == S_OK);
        CAPTURE(configure_D3D11_DXGI(transform.get(), device_manager.get()));
        {
            com_ptr<IMFVideoProcessorControl> control{};
            REQUIRE(transform->QueryInterface(control.put()) == S_OK);
            REQUIRE(control->SetMirror(MIRROR_NONE) == S_OK);
            REQUIRE(control->SetRotation(ROTATION_NONE) == S_OK);
            RECT region{0, 0, 1280, 720};
            REQUIRE(control->SetDestinationRectangle(&region) == S_OK);
        }
        print(transform.get(), CLSID_VideoProcessorMFT);
    }
}
