/**
 * @file    source_test.cpp
 * @author  github.com/luncliff (luncliff@gmail.com)
 */
#include <media.hpp>
#define CATCH_CONFIG_WINDOWS_CRTDBG
#include <catch2/catch.hpp>
#include <spdlog/spdlog.h>

using namespace std;
namespace fs = std::filesystem;

fs::path get_asset_dir() noexcept;
HRESULT create_test_sink_writer(IMFSinkWriterEx** writer, const fs::path& fpath);

// there might be no device in test environment. if the case, it's an expected failure
TEST_CASE("IMFActivate(IMFMediaSourceEx,IMFMediaType)", "[!mayfail]") {
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

    spdlog::info("webcam:");
    for (com_ptr<IMFActivate> device : devices) {
        com_ptr<IMFMediaSourceEx> source{};
        // https://docs.microsoft.com/en-us/windows/win32/api/mfobjects/nf-mfobjects-imfactivate-activateobject
        if (auto hr = device->ActivateObject(__uuidof(IMFMediaSourceEx), source.put_void()))
            FAIL(hr);
        auto after_loop = gsl::finally([source]() { source->Shutdown(); });

        com_ptr<IMFAttributes> attrs{};
        if (auto hr = source->GetSourceAttributes(attrs.put()))
            FAIL(hr);

        GUID capture{};
        if (auto hr = attrs->GetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &capture))
            FAIL(hr);
        REQUIRE(capture == MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

        print(device.get());
        com_ptr<IMFSourceReader> reader{};
        if (auto ec = MFCreateSourceReaderFromMediaSource(source.get(), nullptr, reader.put()))
            FAIL(ec);

        com_ptr<IMFMediaType> source_type{};
        REQUIRE(reader->GetNativeMediaType(0, 0, source_type.put()) == S_OK);
        print(source_type.get());
    }
}

HRESULT get_test_device(com_ptr<IMFActivate>& device) {
    com_ptr<IMFAttributes> attrs{};
    if (auto hr = MFCreateAttributes(attrs.put(), 2))
        return hr;
    if (auto hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))
        return hr;
    std::vector<com_ptr<IMFActivate>> devices{};

    if (auto hr = get_devices(devices, attrs.get()))
        return hr;
    if (devices.empty())
        return E_FAIL;

    device = devices.back();
    return S_OK;
}

TEST_CASE("IMFActivate(IMFSourceReader)", "[!mayfail]") {
    auto on_return = media_startup();

    com_ptr<IMFActivate> device{};
    REQUIRE(get_test_device(device) == S_OK);
    com_ptr<IMFMediaSourceEx> source{};
    REQUIRE(device->ActivateObject(__uuidof(IMFMediaSourceEx), source.put_void()) == S_OK);
    auto on_return2 = gsl::finally([source]() { source->Shutdown(); });

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
        REQUIRE(device_manager->ResetDevice(nullptr, device_manager_token) == E_INVALIDARG);

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

TEST_CASE("IMFActivate(IMFSourceReaderCallback) - 1", "[!mayfail]") {
    auto on_return = media_startup();

    com_ptr<IMFActivate> device{};
    REQUIRE(get_test_device(device) == S_OK);
    com_ptr<IMFMediaSourceEx> source{};
    REQUIRE(device->ActivateObject(__uuidof(IMFMediaSourceEx), source.put_void()) == S_OK);
    auto on_return2 = gsl::finally([device]() { device->ShutdownObject(); });

    spdlog::info("async_source_reader_callback:");
    com_ptr<IMFSourceReaderCallback> callback{};
    REQUIRE(create_reader_callback(callback.put()) == S_OK);

    com_ptr<IMFSourceReader> reader{};
    REQUIRE(create_source_reader(source, callback, reader.put()) == S_OK);

    const auto reader_stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

    com_ptr<IMFMediaType> source_type{};
    REQUIRE(reader->GetNativeMediaType(reader_stream, 0, source_type.put()) == S_OK);
    print(source_type.get());

    REQUIRE(reader->ReadSample(reader_stream, 0, NULL, NULL, NULL, NULL) == S_OK);
    REQUIRE(reader->ReadSample(reader_stream, 0, NULL, NULL, NULL, NULL) == S_OK);
    REQUIRE(reader->ReadSample(reader_stream, 0, NULL, NULL, NULL, NULL) == S_OK);
    SleepEx(1'500, true);
    REQUIRE(reader->Flush(reader_stream) == S_OK);
    SleepEx(500, true);
}

TEST_CASE("IMFActivate(IMFSourceReaderCallback) - 2", "[!mayfail]") {
    auto on_return = media_startup();

    com_ptr<IMFActivate> device{};
    REQUIRE(get_test_device(device) == S_OK);
    com_ptr<IMFMediaSourceEx> source{};
    REQUIRE(device->ActivateObject(__uuidof(IMFMediaSourceEx), source.put_void()) == S_OK);
    auto on_return2 = gsl::finally([device]() { device->ShutdownObject(); });

    com_ptr<IMFSourceReaderCallback> callback{};
    REQUIRE(create_reader_callback(callback.put()) == S_OK);

    com_ptr<IMFSourceReader> reader{};
    REQUIRE(create_source_reader(source, callback, reader.put()) == S_OK);

    const auto reader_stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

    SECTION("Flush immediately") {
        REQUIRE(reader->Flush(reader_stream) == S_OK);
        SleepEx(500, true);
    }
    SECTION("MFVideoFormat_NV12(1920/1080)") {
        com_ptr<IMFMediaType> native_type{};
        REQUIRE(reader->GetNativeMediaType(reader_stream, 0, native_type.put()) == S_OK);

        com_ptr<IMFMediaType> output_type{};
        REQUIRE(MFCreateMediaType(output_type.put()) == S_OK);
        REQUIRE(native_type->CopyAllItems(output_type.get()) == S_OK);
        REQUIRE(output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12) == S_OK);
        REQUIRE(MFSetAttributeSize(output_type.get(), MF_MT_FRAME_SIZE, 1920, 1080) == S_OK); // -> 3110400
        REQUIRE(MFSetAttributeRatio(output_type.get(), MF_MT_FRAME_RATE, 30, 1) == S_OK);
        REQUIRE(reader->SetCurrentMediaType(reader_stream, NULL, output_type.get()) == S_OK);

        REQUIRE(reader->ReadSample(reader_stream, 0, NULL, NULL, NULL, NULL) == S_OK);
        REQUIRE(reader->ReadSample(reader_stream, 0, NULL, NULL, NULL, NULL) == S_OK);
        REQUIRE(reader->ReadSample(reader_stream, 0, NULL, NULL, NULL, NULL) == S_OK);
        SleepEx(1'500, true);
        REQUIRE(reader->Flush(reader_stream) == S_OK);
        SleepEx(500, true);
    }
    SECTION("MFVideoFormat_RGB32(1920/1080)") {
        com_ptr<IMFMediaType> native_type{};
        REQUIRE(reader->GetNativeMediaType(reader_stream, 0, native_type.put()) == S_OK);

        com_ptr<IMFMediaType> output_type{};
        REQUIRE(MFCreateMediaType(output_type.put()) == S_OK);
        REQUIRE(native_type->CopyAllItems(output_type.get()) == S_OK);
        REQUIRE(output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32) == S_OK);
        REQUIRE(MFSetAttributeSize(output_type.get(), MF_MT_FRAME_SIZE, 1920, 1080) == S_OK); // -> 3110400
        REQUIRE(MFSetAttributeRatio(output_type.get(), MF_MT_FRAME_RATE, 30, 1) == S_OK);
        REQUIRE(reader->SetCurrentMediaType(reader_stream, NULL, output_type.get()) == S_OK);

        REQUIRE(reader->ReadSample(reader_stream, 0, NULL, NULL, NULL, NULL) == S_OK);
        REQUIRE(reader->ReadSample(reader_stream, 0, NULL, NULL, NULL, NULL) == S_OK);
        REQUIRE(reader->ReadSample(reader_stream, 0, NULL, NULL, NULL, NULL) == S_OK);
        SleepEx(1'500, true);
        REQUIRE(reader->Flush(reader_stream) == S_OK);
        SleepEx(500, true);
    }
}
