/**
 * @file    source_test.cpp
 * @author  github.com/luncliff (luncliff@gmail.com)
 */
#define CATCH_CONFIG_WINDOWS_CRTDBG
#include <catch2/catch.hpp>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.Threading.h>

#include <media.hpp>

using namespace std;
namespace fs = std::filesystem;

fs::path get_asset_dir() noexcept;

// there might be no device in test environment. if the case, it's an expected failure
TEST_CASE("IMFActivate(IMFMediaSourceEx)", "[!mayfail]") {
    auto on_return = media_startup();

    std::vector<com_ptr<IMFActivate>> devices{};
    {
        com_ptr<IMFAttributes> attrs{};
        REQUIRE(MFCreateAttributes(attrs.put(), 1) == S_OK);
        REQUIRE(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID) ==
                S_OK);
        REQUIRE(get_devices(devices, attrs.get()) == S_OK);
    }
    REQUIRE(devices.size() > 0);

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

TEST_CASE("IMFActivate(IMFSourceReader)", "[!mayfail]") {
    auto on_return = media_startup();

    std::vector<com_ptr<IMFActivate>> devices{};
    {
        com_ptr<IMFAttributes> attrs{};
        REQUIRE(MFCreateAttributes(attrs.put(), 2) == S_OK);
        REQUIRE(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID) ==
                S_OK);
        REQUIRE(get_devices(devices, attrs.get()) == S_OK);
    }
    REQUIRE(devices.size() > 0);

    com_ptr<IMFMediaSourceEx> source{};
    REQUIRE(devices[0]->ActivateObject(__uuidof(IMFMediaSourceEx), source.put_void()) == S_OK);

    SECTION("custom - 1") {
        com_ptr<IMFAttributes> attrs{};
        REQUIRE(MFCreateAttributes(attrs.put(), 2) == S_OK);
        REQUIRE(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE) == S_OK);
        REQUIRE(attrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE) == S_OK);
        com_ptr<IMFSourceReader> reader{};
        REQUIRE(MFCreateSourceReaderFromMediaSource(source.get(), attrs.get(), reader.put()) == S_OK);
    }
    SECTION("IMFDXGIDeviceManager") {

        UINT device_manager_token{};
        com_ptr<IMFDXGIDeviceManager> device_manager{};
        REQUIRE(MFCreateDXGIDeviceManager(&device_manager_token, device_manager.put()) == S_OK);

        com_ptr<IMFAttributes> attrs{};
        REQUIRE(MFCreateAttributes(attrs.put(), 3) == S_OK);
        REQUIRE(attrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, nullptr) == S_OK);
        REQUIRE(attrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE) == S_OK);
        REQUIRE(attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, device_manager.get()) == S_OK);
        com_ptr<IMFSourceReader> reader{};
        REQUIRE(MFCreateSourceReaderFromMediaSource(source.get(), attrs.get(), reader.put()) == S_OK);
    }

    SECTION("IMFSourceReaderEx") {
        com_ptr<IMFSourceReader> reader{};
        REQUIRE(MFCreateSourceReaderFromMediaSource(source.get(), nullptr, reader.put()) == S_OK);
        com_ptr<IMFSourceReaderEx> reader_ex{};
        REQUIRE(reader->QueryInterface(reader_ex.put()) == S_OK);
    }
}
