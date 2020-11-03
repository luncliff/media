#define CATCH_CONFIG_WINDOWS_CRTDBG
#include <catch2/catch.hpp>

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

using namespace std;
using namespace Microsoft::WRL;

namespace fs = std::filesystem;

fs::path get_asset_dir() noexcept;

auto media_open() {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    if (MFStartup(MF_VERSION) != S_OK)
        throw runtime_error{"MFStartup"};
    return gsl::finally([]() {
        MFShutdown();
        winrt::uninit_apartment();
    });
}

void make_media_source(IMFMediaSource** media_source, const fs::path& fpath) {
    REQUIRE(fs::exists(fpath));

    ComPtr<IMFSourceResolver> resolver{};
    REQUIRE(MFCreateSourceResolver(resolver.GetAddressOf()) == S_OK);
    MF_OBJECT_TYPE source_type = MF_OBJECT_INVALID;
    ComPtr<IUnknown> source{};
    REQUIRE(resolver->CreateObjectFromURL(fpath.c_str(),             // URL of the source.
                                          MF_RESOLUTION_MEDIASOURCE, // Create a source object.
                                          NULL,                      // Optional property store.
                                          &source_type,              // Receives the created object type.
                                          source.GetAddressOf()      // Receives a pointer to the media source.
                                          ) == S_OK);
    REQUIRE(source_type == MF_OBJECT_MEDIASOURCE);
    REQUIRE(source->QueryInterface(IID_PPV_ARGS(media_source)) == S_OK);
}

void make_souce_reader(IMFSourceReader** source_reader, IMFMediaSource* media_source) {
    ComPtr<IMFAttributes> reader_attributes{};
    REQUIRE(MFCreateAttributes(reader_attributes.GetAddressOf(), 2) == S_OK);
    REQUIRE(reader_attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                       MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID) == S_OK);
    REQUIRE(reader_attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, 1) == S_OK);
    REQUIRE(MFCreateSourceReaderFromMediaSource(media_source, reader_attributes.Get(), source_reader) == S_OK);
}

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/h-264-video-decoder
void make_video_transform(IMFTransform** decoding_transform, IMFSourceReader* source_reader,
                          IMFMediaType** source_type) {
    // @todo CLSID_CMSMPEGDecoderMFT, CLSID_VideoProcessorMFT, CLSID_CMSH264DecoderMFT, IID_IMFTransform, IID_IUnknown
    ComPtr<IUnknown> transform{};
    REQUIRE(CoCreateInstance(CLSID_CMSH264DecoderMFT, NULL, CLSCTX_INPROC_SERVER, IID_IMFTransform,
                             (void**)transform.GetAddressOf()) == S_OK);
    REQUIRE(transform->QueryInterface(IID_PPV_ARGS(decoding_transform)) == S_OK);

    REQUIRE(source_reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, source_type) == S_OK);
    GUID major{};
    REQUIRE((*source_type)->GetMajorType(&major) == S_OK);
    REQUIRE(major == MFMediaType_Video);
}

void check_device(IMFDXGIDeviceManager* manager, ID3D11Device* expected) {
    HANDLE handle{};
    REQUIRE(manager->OpenDeviceHandle(&handle) == S_OK);
    ComPtr<ID3D11Device> device{};
    REQUIRE(manager->GetVideoService(handle, IID_ID3D11Device, (void**)device.GetAddressOf()) == S_OK);
    REQUIRE(device.Get() == expected);
}

void check_device(IMFDXGIDeviceManager* manager, ID3D11VideoDevice* expected) {
    HANDLE handle{};
    REQUIRE(manager->OpenDeviceHandle(&handle) == S_OK);
    ComPtr<ID3D11VideoDevice> device{};
    REQUIRE(manager->GetVideoService(handle, IID_ID3D11VideoDevice, (void**)device.GetAddressOf()) == S_OK);
    REQUIRE(device.Get() == expected);
}

void check_device_context(ID3D11Device* device, ID3D11DeviceContext* expected) {
    ComPtr<ID3D11DeviceContext> context{};
    device->GetImmediateContext(context.GetAddressOf());
    REQUIRE(context.Get() == expected);
}

void configure_multithread_protection(ID3D11Device* device) {
    ComPtr<ID3D10Multithread> protection{};
    REQUIRE(device->QueryInterface(__uuidof(ID3D10Multithread), (void**)protection.GetAddressOf()) == S_OK);
    CAPTURE(protection->SetMultithreadProtected(TRUE));
}

/// @see source reader scenario in https://docs.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/direct3d-aware-mfts
/// @see https://github.com/microsoft/Windows-classic-samples/tree/master/Samples/DX11VideoRenderer
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/basic-mft-processing-model#process-data
TEST_CASE("MFTransform with Direct11 Device", "[!mayfail]") {
    auto on_exit = media_open();

    // To perform decoding using Direct3D 11, the software decoder must have a pointer to a Direct3D 11 device.
    ComPtr<ID3D11Device> graphics_device{};
    ComPtr<ID3D11DeviceContext> graphics_device_context{};
    {
        D3D_FEATURE_LEVEL level{};
        D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        REQUIRE(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 3,
                                  D3D11_SDK_VERSION, graphics_device.GetAddressOf(), &level,
                                  graphics_device_context.GetAddressOf()) == S_OK);
    }
    // multi-thread protection
    configure_multithread_protection(graphics_device.Get());

    // get video accelerator to ensure D3D11_CREATE_DEVICE_VIDEO_SUPPORT
    ComPtr<ID3D11VideoDevice> video_device{};
    REQUIRE(graphics_device->QueryInterface(__uuidof(ID3D11VideoDevice), //
                                            (void**)video_device.GetAddressOf()) == S_OK);
    ComPtr<ID3D11VideoContext> video_context{};
    REQUIRE(graphics_device_context->QueryInterface(__uuidof(ID3D11VideoContext), //
                                                    (void**)video_context.GetAddressOf()) == S_OK);

    // The DXGI Device Manager is used to share the Direct3D 11 between components.
    UINT device_manager_token{};
    ComPtr<IMFDXGIDeviceManager> device_manager{};
    REQUIRE(MFCreateDXGIDeviceManager(&device_manager_token, device_manager.GetAddressOf()) == S_OK);
    REQUIRE(device_manager->ResetDevice(graphics_device.Get(), device_manager_token) == S_OK);

    check_device(device_manager.Get(), graphics_device.Get());
    check_device(device_manager.Get(), video_device.Get());
    check_device_context(graphics_device.Get(), graphics_device_context.Get());

    SECTION("IMFSourceReader/IMFTransform") {
        ComPtr<IMFMediaSource> media_source{};
        make_media_source(media_source.GetAddressOf(), fs::absolute(get_asset_dir() / "fm5p7flyCSY.mp4"));
        ComPtr<IMFSourceReader> source_reader{};
        make_souce_reader(source_reader.GetAddressOf(), media_source.Get());

        ComPtr<IMFTransform> transform{};
        ComPtr<IMFMediaType> source_type{};
        make_video_transform(transform.GetAddressOf(), source_reader.Get(), source_type.GetAddressOf());

        // configure Direct3D 11
        ComPtr<IMFAttributes> transform_attributes{};
        REQUIRE(transform->GetAttributes(transform_attributes.GetAddressOf()) == S_OK);
        REQUIRE(transform_attributes->SetUINT32(MF_SA_D3D_AWARE, TRUE) == S_OK);
        REQUIRE(transform_attributes->SetUINT32(MF_SA_D3D11_AWARE, TRUE) == S_OK);
        REQUIRE(transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, //
                                          reinterpret_cast<ULONG_PTR>(static_cast<IUnknown*>(device_manager.Get()))) ==
                S_OK);

        SECTION("ProcessMessage") {
            ComPtr<IMFMediaType> input_media_type{};
            REQUIRE(MFCreateMediaType(input_media_type.ReleaseAndGetAddressOf()) == S_OK);
            REQUIRE(source_type->CopyAllItems(input_media_type.Get()) == S_OK);
            if (auto hr = transform->SetInputType(0, input_media_type.Get(), 0))
                FAIL(hr);

            ComPtr<IMFMediaType> output_media_type{};
            REQUIRE(MFCreateMediaType(output_media_type.ReleaseAndGetAddressOf()) == S_OK);
            REQUIRE(source_type->CopyAllItems(output_media_type.Get()) == S_OK);
            //  MFVideoFormat_NV12, MFVideoFormat_YV12 MFVideoFormat_IYUV MFVideoFormat_I420 MFVideoFormat_YUY2
            if (auto hr = output_media_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_I420))
                FAIL(hr);

            REQUIRE(transform->SetOutputType(0, output_media_type.Get(), 0) == S_OK);

            // configure leftovers
            REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL) == S_OK);
            REQUIRE(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL) == S_OK);
            REQUIRE(transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL) == S_OK);

            MFT_OUTPUT_STREAM_INFO output_stream_info{};
            output_stream_info.dwFlags = MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;
            REQUIRE(transform->GetOutputStreamInfo(0, &output_stream_info) == S_OK);
            ComPtr<IMFAttributes> output_stream_attributes{};
            REQUIRE(transform->GetOutputStreamAttributes(0, output_stream_attributes.GetAddressOf()) == S_OK);

            /// @see https://docs.microsoft.com/en-us/windows/win32/api/mfapi/nf-mfapi-mfcreatedxsurfacebuffer
            SECTION("Allocating Uncompressed Buffers") {
                //D3D11_VIDEO_PROCESSOR_CAPS caps{};
                D3D11_SUBRESOURCE_DATA subdata{};
                D3D11_TEXTURE2D_DESC desc{};
                ComPtr<ID3D11Texture2D> texture{};
                REQUIRE(graphics_device->CreateTexture2D(&desc, &subdata, texture.GetAddressOf()) == S_OK);

                ComPtr<IMFMediaBuffer> surface_buffer{};
                REQUIRE(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture.Get(), 1, FALSE,
                                                  surface_buffer.GetAddressOf()) == S_OK);

                ComPtr<IMFSample> sample{};
                //REQUIRE(MFCreateVideoSampleFromSurface(nullptr, sample.GetAddressOf()) == S_OK);
                REQUIRE(MFCreateSample(sample.GetAddressOf()) == S_OK);
                REQUIRE(sample->AddBuffer(surface_buffer.Get()) == S_OK);

                D3D11_VIDEO_DECODER_DESC decoder_desc{};
                D3D11_VIDEO_DECODER_CONFIG decoder_config{};
                ComPtr<ID3D11VideoDecoder> decoder{};
                REQUIRE(video_device->CreateVideoDecoder(&decoder_desc, &decoder_config, decoder.GetAddressOf()) ==
                        S_OK);
            }
        }

        // then configure input/output
        SECTION("GetInputAvailableType") {
            ComPtr<IMFMediaType> input_media_type{};
            for (DWORD i = 0; transform->GetInputAvailableType(0, i, input_media_type.ReleaseAndGetAddressOf()) == S_OK;
                 ++i) {
                // MFVideoFormat_RGB24. MFVideoFormat_RGB32, MFVideoFormat_ARGB32, MFVideoFormat_RGB565
                GUID subtype{};
                input_media_type->GetGUID(MF_MT_SUBTYPE, &subtype);
                auto name = get_name(subtype);
                if (name == nullptr)
                    FAIL("unknown MF_MT_SUBTYPE detected");
                std::cout << "- input: " << name << std::endl;

                if (auto hr = transform->SetInputType(0, input_media_type.Get(), 0))
                    FAIL(hr);

                std::cout << "  output: " << std::endl;
                ComPtr<IMFMediaType> output_media_type{};
                for (DWORD i = 0;
                     transform->GetOutputAvailableType(0, i, output_media_type.ReleaseAndGetAddressOf()) == S_OK; ++i) {
                    output_media_type->GetGUID(MF_MT_SUBTYPE, &subtype);
                    if (auto name = get_name(subtype))
                        std::cout << "   - " << name << std::endl;
                }
            }
        }
    }

    SECTION("Find a Decoder Configuration") {
        //D3D11_VIDEO_DECODER_DESC decoder_desc{};
        //D3D11_VIDEO_DECODER_CONFIG decoder_config{};
        //ComPtr<ID3D11VideoDecoder> decoder{};
        //REQUIRE(video_device->CreateVideoDecoder(&decoder_desc, &decoder_config, decoder.GetAddressOf()) == S_OK);

        for (auto i = 0u; i < video_device->GetVideoDecoderProfileCount(); ++i) {
            GUID profile{};
            if (auto hr = video_device->GetVideoDecoderProfile(i, &profile))
                FAIL(hr);

            // MPEG-1/2
            if (profile == D3D11_DECODER_PROFILE_MPEG1_VLD || profile == D3D11_DECODER_PROFILE_MPEG2_MOCOMP ||
                profile == D3D11_DECODER_PROFILE_MPEG2_IDCT || profile == D3D11_DECODER_PROFILE_MPEG2_VLD ||
                profile == D3D11_DECODER_PROFILE_MPEG2and1_VLD) {
            }
            // ITU-T H.264/MPEG-4 Part 10, AVC (ISO/IEC 14496-10)
            else if (profile == D3D11_DECODER_PROFILE_H264_MOCOMP_NOFGT ||
                     profile == D3D11_DECODER_PROFILE_H264_MOCOMP_FGT ||
                     profile == D3D11_DECODER_PROFILE_H264_IDCT_NOFGT ||
                     profile == D3D11_DECODER_PROFILE_H264_IDCT_FGT ||
                     profile == D3D11_DECODER_PROFILE_H264_VLD_NOFGT || profile == D3D11_DECODER_PROFILE_H264_VLD_FGT ||
                     profile == D3D11_DECODER_PROFILE_H264_VLD_WITHFMOASO_NOFGT ||
                     profile == D3D11_DECODER_PROFILE_H264_VLD_STEREO_PROGRESSIVE_NOFGT ||
                     profile == D3D11_DECODER_PROFILE_H264_VLD_STEREO_NOFGT ||
                     profile == D3D11_DECODER_PROFILE_H264_VLD_MULTIVIEW_NOFGT) {
            }
            // ITU-T H.265/MPEG-H Part 2, HEVC (ISO/IEC 23008-2)
            else if (profile == D3D11_DECODER_PROFILE_HEVC_VLD_MAIN ||
                     profile == D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10) {
            }
            // Windows Media Video
            else if (profile == D3D11_DECODER_PROFILE_WMV8_POSTPROC || profile == D3D11_DECODER_PROFILE_WMV8_MOCOMP ||
                     profile == D3D11_DECODER_PROFILE_WMV9_POSTPROC || profile == D3D11_DECODER_PROFILE_WMV9_MOCOMP ||
                     profile == D3D11_DECODER_PROFILE_WMV9_IDCT) {

            }
            // SMPTE 421M (VC-1)
            else if (profile == D3D11_DECODER_PROFILE_VC1_POSTPROC || profile == D3D11_DECODER_PROFILE_VC1_MOCOMP ||
                     profile == D3D11_DECODER_PROFILE_VC1_IDCT || profile == D3D11_DECODER_PROFILE_VC1_VLD ||
                     profile == D3D11_DECODER_PROFILE_VC1_D2010) {
            }
            // MPEG-4 Part 2, MPEG-4 Visual (ISO/IEC 14496-2)
            else if (profile == D3D11_DECODER_PROFILE_MPEG4PT2_VLD_SIMPLE ||
                     profile == D3D11_DECODER_PROFILE_MPEG4PT2_VLD_ADVSIMPLE_NOGMC ||
                     profile == D3D11_DECODER_PROFILE_MPEG4PT2_VLD_ADVSIMPLE_GMC) {
            } else {
                FAIL("unexpected profile detected");
            }

            BOOL supported{};
            /// @see https://docs.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11videodevice-checkvideodecoderformat
            switch (auto hr = video_device->CheckVideoDecoderFormat(&profile, DXGI_FORMAT_420_OPAQUE, &supported)) {
            case E_INVALIDARG:
                std::cerr << "the profile is not supported." << std::endl;
                continue;
            default:
                if (supported == false)
                    continue;
            }

            // D3D11_VIDEO_DECODER_DESC desc{};
            // UINT count{};
            // if (auto hr = video_device->GetVideoDecoderConfigCount(&desc, &count))
            //     FAIL(hr);
            // for (auto i = 0u; i < count; ++i) {
            //     D3D11_VIDEO_DECODER_CONFIG config{};
            //     if (auto hr = video_device->GetVideoDecoderConfig(&desc, i, &config))
            //         FAIL(hr);
            // }
        }
    }
}

HRESULT check_sample(ComPtr<IMFSample> sample);

TEST_CASE("MFTransform(MP4-YUV) with ID3D11Device") {
    const auto fpath = fs::absolute(get_asset_dir() / "fm5p7flyCSY.mp4");
    auto on_return = media_startup();

    // To perform decoding using Direct3D 11, the software decoder must have a pointer to a Direct3D 11 device.
    ComPtr<ID3D11Device> graphics_device{};
    ComPtr<ID3D11DeviceContext> graphics_device_context{};
    {
        D3D_FEATURE_LEVEL level{};
        D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        REQUIRE(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 3,
                                  D3D11_SDK_VERSION, graphics_device.GetAddressOf(), &level,
                                  graphics_device_context.GetAddressOf()) == S_OK);
    }
    // multi-thread protection
    configure_multithread_protection(graphics_device.Get());

    // get video accelerator to ensure D3D11_CREATE_DEVICE_VIDEO_SUPPORT
    ComPtr<ID3D11VideoDevice> video_device{};
    REQUIRE(graphics_device->QueryInterface(__uuidof(ID3D11VideoDevice), //
                                            (void**)video_device.GetAddressOf()) == S_OK);
    ComPtr<ID3D11VideoContext> video_context{};
    REQUIRE(graphics_device_context->QueryInterface(__uuidof(ID3D11VideoContext), //
                                                    (void**)video_context.GetAddressOf()) == S_OK);

    // The DXGI Device Manager is used to share the Direct3D 11 between components.
    UINT device_manager_token{};
    ComPtr<IMFDXGIDeviceManager> device_manager{};
    REQUIRE(MFCreateDXGIDeviceManager(&device_manager_token, device_manager.GetAddressOf()) == S_OK);
    REQUIRE(device_manager->ResetDevice(graphics_device.Get(), device_manager_token) == S_OK);

    check_device(device_manager.Get(), graphics_device.Get());
    check_device(device_manager.Get(), video_device.Get());
    check_device_context(graphics_device.Get(), graphics_device_context.Get());

    MF_OBJECT_TYPE media_object_type = MF_OBJECT_INVALID;
    ComPtr<IMFSourceResolver> resolver{};
    ComPtr<IUnknown> source{};
    REQUIRE(MFCreateSourceResolver(resolver.GetAddressOf()) == S_OK);
    REQUIRE(resolver->CreateObjectFromURL(fpath.c_str(),             // URL of the source.
                                          MF_RESOLUTION_MEDIASOURCE, // Create a source object.
                                          NULL,                      // Optional property store.
                                          &media_object_type,        // Receives the created object type.
                                          source.GetAddressOf()      // Receives a pointer to the media source.
                                          ) == S_OK);

    ComPtr<IMFMediaSource> media_source{};
    REQUIRE(source->QueryInterface(IID_PPV_ARGS(media_source.GetAddressOf())) == S_OK);

    ComPtr<IMFAttributes> reader_attributes{};
    REQUIRE(MFCreateAttributes(reader_attributes.GetAddressOf(), 2) == S_OK);
    REQUIRE(reader_attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                       MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID) == S_OK);
    REQUIRE(reader_attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, 1) == S_OK);

    ComPtr<IMFSourceReader> source_reader{};
    REQUIRE(MFCreateSourceReaderFromMediaSource(media_source.Get(), reader_attributes.Get(),
                                                source_reader.GetAddressOf()) == S_OK);

    ComPtr<IMFMediaType> file_video_media_type{};
    REQUIRE(source_reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                               file_video_media_type.GetAddressOf()) == S_OK);

    ComPtr<IUnknown> transform{};
    ComPtr<IMFTransform> decoding_transform{}; // This is H264 Decoder MFT
    REQUIRE(CoCreateInstance(CLSID_CMSH264DecoderMFT, NULL, CLSCTX_INPROC_SERVER, IID_IUnknown,
                             (void**)transform.GetAddressOf()) == S_OK);
    REQUIRE(transform->QueryInterface(IID_PPV_ARGS(decoding_transform.GetAddressOf())) == S_OK);

    // configure Direct3D 11
    ComPtr<IMFAttributes> transform_attributes{};
    REQUIRE(decoding_transform->GetAttributes(transform_attributes.GetAddressOf()) == S_OK);
    REQUIRE(transform_attributes->SetUINT32(MF_SA_D3D_AWARE, TRUE) == S_OK);
    REQUIRE(transform_attributes->SetUINT32(MF_SA_D3D11_AWARE, TRUE) == S_OK);
    REQUIRE(decoding_transform->ProcessMessage(
                MFT_MESSAGE_SET_D3D_MANAGER, //
                reinterpret_cast<ULONG_PTR>(static_cast<IUnknown*>(device_manager.Get()))) == S_OK);

    ComPtr<IMFMediaType> input_media_type{};
    REQUIRE(MFCreateMediaType(input_media_type.GetAddressOf()) == S_OK);
    REQUIRE(file_video_media_type->CopyAllItems(input_media_type.Get()) == S_OK);
    if (auto hr = decoding_transform->SetInputType(0, input_media_type.Get(), 0))
        FAIL(hr);

    ComPtr<IMFMediaType> output_media_type{};
    REQUIRE(MFCreateMediaType(output_media_type.GetAddressOf()) == S_OK);
    REQUIRE(file_video_media_type->CopyAllItems(output_media_type.Get()) == S_OK);
    if (auto hr = output_media_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_IYUV))
        FAIL(hr);

    if (auto hr = decoding_transform->SetOutputType(0, output_media_type.Get(), 0))
        FAIL(hr);

    DWORD status = 0;
    REQUIRE(decoding_transform->GetInputStatus(0, &status) == S_OK);
    REQUIRE(status == MFT_INPUT_STATUS_ACCEPT_DATA);
    //CAPTURE(GetMediaTypeDescription(output_media_type.Get()));
    REQUIRE(decoding_transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL) == S_OK);
    REQUIRE(decoding_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL) == S_OK);
    REQUIRE(decoding_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL) == S_OK);

    size_t count = 0;
    try {
        for (auto decoded_sample : decode(source_reader, decoding_transform)) {
            if (decoded_sample == nullptr)
                FAIL();
            ++count;
            if (auto hr = check_sample(decoded_sample))
                FAIL(hr);
        }
    } catch (const winrt::hresult_error& ex) {
        // FAIL(ex.message());
        FAIL(ex.code());
    }
    REQUIRE(count > 0);
}
