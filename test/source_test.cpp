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
    const auto reader_stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    REQUIRE(source_reader->GetCurrentMediaType(reader_stream, source_type.put()) == S_OK);
    REQUIRE(source_type->SetGUID(MF_MT_SUBTYPE, output_subtype) == S_OK);
    switch (auto hr = source_reader->SetCurrentMediaType(reader_stream, NULL, source_type.get())) {
    case S_OK:
        break;
    case MF_E_INVALIDMEDIATYPE:
        FAIL("MF_E_INVALIDMEDIATYPE");
    default:
        FAIL(to_readable(hr));
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
        FAIL(to_readable(hr)); // MF_E_UNSUPPORTED_SCHEME, MF_E_SOURCERESOLVER_MUTUALLY_EXCLUSIVE_FLAGS
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
            FAIL(to_readable(ec));

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
    const auto reader_stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    REQUIRE(source_reader->GetCurrentMediaType(reader_stream, source_type.put()) == S_OK);

    // make a copy instead of modification
    com_ptr<IMFMediaType> output_type{};
    REQUIRE(MFCreateMediaType(output_type.put()) == S_OK);
    REQUIRE(source_type->CopyAllItems(output_type.get()) == S_OK);

    SECTION("MF_MT_SUBTYPE(MFVideoFormat_NV12)") {
        REQUIRE(output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12) == S_OK);
        REQUIRE(source_reader->SetCurrentMediaType(reader_stream, NULL, output_type.get()) == S_OK);
    }
    SECTION("MF_MT_SUBTYPE(MFVideoFormat_RGB32)") {
        REQUIRE(output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32) == S_OK);
        REQUIRE(source_reader->SetCurrentMediaType(reader_stream, NULL, output_type.get()) == S_OK);
    }
    // The followings may not pass. Will be resolved in future
    SECTION("MF_MT_INTERLACE_MODE") {
        REQUIRE(output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Unknown) == S_OK);
        const auto hr = source_reader->SetCurrentMediaType(reader_stream, NULL, output_type.get());
        REQUIRE(FAILED(hr));
    }
    SECTION("MF_MT_FRAME_RATE/MF_MT_FRAME_SIZE") {
        REQUIRE(MFSetAttributeRatio(output_type.get(), MF_MT_FRAME_RATE, 30, 1) == S_OK);    // 30 fps
        REQUIRE(MFSetAttributeSize(output_type.get(), MF_MT_FRAME_SIZE, 1280, 720) == S_OK); // w 1280 h 720
        const auto hr = source_reader->SetCurrentMediaType(reader_stream, NULL, output_type.get());
        REQUIRE(FAILED(hr));
    }
}
