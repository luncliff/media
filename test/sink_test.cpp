/**
 * @file    sink_test.cpp
 * @author  github.com/luncliff (luncliff@gmail.com)
 * @see     https://docs.microsoft.com/en-us/windows/win32/medfound/sink-writer
 * @see     https://docs.microsoft.com/en-us/windows/win32/api/mfreadwrite/nn-mfreadwrite-imfsinkwriter
 */
#define CATCH_CONFIG_WINDOWS_CRTDBG
#include <catch2/catch.hpp>
#include <future>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.Threading.h>

#include <media.hpp>

#include <d3d11.h>
#include <d3d9.h>
#include <windows.h>
#include <windowsx.h>

#include <Dxva2api.h>
#include <evr.h>
#include <mfplay.h>
#include <mfreadwrite.h>

#pragma comment(lib, "strmiids") // for MR_VIDEO_RENDER_SERVICE

TEST_CASE("MFCreateMPEG4MediaSink") {
    auto on_return = media_startup();

    com_ptr<IMFByteStream> byte_stream{};
    auto fpath = fs::current_path() / "output1.mp4";
    if (auto ec = MFCreateFile(MF_ACCESSMODE_WRITE, MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NOBUFFERING,
                               fpath.c_str(), byte_stream.put()))
        FAIL(ec);

    // todo: add MF_MT_FRAME_SIZE, MF_MT_PIXEL_ASPECT_RATIO?
    com_ptr<IMFMediaType> video_type{};
    REQUIRE(MFCreateMediaType(video_type.put()) == S_OK);
    REQUIRE(video_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video) == S_OK);
    REQUIRE(video_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264) == S_OK); // --> .mp4
    REQUIRE(video_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive) == S_OK);
    REQUIRE(video_type->SetUINT32(MF_MT_AVG_BITRATE, 800'000) == S_OK);
    REQUIRE(MFSetAttributeRatio(video_type.get(), MF_MT_FRAME_RATE, 30, 1) == S_OK); // 30 fps

    com_ptr<IMFMediaSink> sink{};
    REQUIRE(MFCreateMPEG4MediaSink(byte_stream.get(), video_type.get(), nullptr, // == NO audio
                                   sink.put()) == S_OK);

    DWORD num_stream_sink = 0;
    REQUIRE(sink->GetStreamSinkCount(&num_stream_sink) == S_OK);
    REQUIRE(num_stream_sink > 0);
}

TEST_CASE("IMFReadWriteClassFactory::CreateInstanceFromURL") {
    auto on_return = media_startup();

    com_ptr<IMFReadWriteClassFactory> factory{};
    REQUIRE(CoCreateInstance(CLSID_MFReadWriteClassFactory, NULL, CLSCTX_INPROC_SERVER, //
                             IID_PPV_ARGS(factory.put())) == S_OK);

    com_ptr<IMFAttributes> attrs{};
    REQUIRE(MFCreateAttributes(attrs.put(), 2) == S_OK);
    REQUIRE(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE) == S_OK);
    REQUIRE(attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, FALSE) == S_OK);

    const auto fpath = fs::current_path() / L"output2.mp4";
    com_ptr<IMFSinkWriter> writer{};
    REQUIRE(factory->CreateInstanceFromURL(CLSID_MFSinkWriter, fpath.c_str(), attrs.get(), //
                                           IID_PPV_ARGS(writer.put())) == S_OK);
    com_ptr<IMFSinkWriterEx> writer_ex{};
    REQUIRE(writer->QueryInterface(writer_ex.put()) == S_OK);
}

// https://docs.microsoft.com/en-us/windows/win32/medfound/mf-readwrite-enable-hardware-transforms
// https://docs.microsoft.com/en-us/windows/win32/medfound/tutorial--using-the-sink-writer-to-encode-video#send-video-frames-to-the-sink-writer
TEST_CASE("MFCreateSinkWriterFromURL(MPEG4)") {
    auto on_return = media_startup();

    com_ptr<IMFAttributes> attrs{};
    REQUIRE(MFCreateAttributes(attrs.put(), 2) == S_OK);
    REQUIRE(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE) == S_OK);
    REQUIRE(attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, FALSE) == S_OK);

    const auto fpath = fs::current_path() / L"output3.mp4";
    com_ptr<IMFSinkWriter> writer{};
    REQUIRE(MFCreateSinkWriterFromURL(fpath.c_str(), NULL, attrs.get(), writer.put()) == S_OK);
    com_ptr<IMFSinkWriterEx> writer_ex{};
    REQUIRE(writer->QueryInterface(writer_ex.put()) == S_OK);
}

HRESULT create_test_sink_writer(IMFSinkWriterEx** writer, const fs::path& fpath) {
    com_ptr<IMFAttributes> attrs{};
    REQUIRE(MFCreateAttributes(attrs.put(), 1) == S_OK);
    REQUIRE(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE) == S_OK);

    com_ptr<IMFSinkWriter> sink_writer{};
    REQUIRE(MFCreateSinkWriterFromURL(fpath.c_str(), NULL, attrs.get(), sink_writer.put()) == S_OK);
    return sink_writer->QueryInterface(writer);
}

// https://docs.microsoft.com/en-us/windows/win32/medfound/using-the-sink-writer
// https://docs.microsoft.com/en-us/windows/win32/medfound/tutorial--using-the-sink-writer-to-encode-video#define-the-video-format
// https://stackoverflow.com/a/24891959
TEST_CASE("IMFSinkWriterEx(MPEG4)") {
    auto on_return = media_startup();

    com_ptr<IMFSinkWriterEx> writer{};
    REQUIRE(create_test_sink_writer(writer.put(), fs::current_path() / L"output2.mp4") == S_OK);

    com_ptr<IMFByteStream> byte_stream{};
    auto fpath = fs::current_path() / "output3.mp4";
    if (auto ec = MFCreateFile(MF_ACCESSMODE_WRITE, MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NOBUFFERING,
                               fpath.c_str(), byte_stream.put()))
        FAIL(ec);

    constexpr auto total_duration = 10'000'000;
    constexpr auto fps = 30;
    constexpr auto frame_duration = total_duration / fps;
    constexpr auto width = 640, height = 480;

    DWORD stream_index = 0;
    com_ptr<IMFMediaType> output_type{};
    {
        REQUIRE(MFCreateMediaType(output_type.put()) == S_OK);
        REQUIRE(output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video) == S_OK);
        REQUIRE(output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264) == S_OK);
        REQUIRE(output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive) == S_OK);
        REQUIRE(output_type->SetUINT32(MF_MT_AVG_BITRATE, 800'000) == S_OK);
        REQUIRE(MFSetAttributeRatio(output_type.get(), MF_MT_FRAME_RATE, fps, 1) == S_OK); // 30 fps
        REQUIRE(MFSetAttributeSize(output_type.get(), MF_MT_FRAME_SIZE, width, height) == S_OK);
        REQUIRE(MFSetAttributeRatio(output_type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1) == S_OK);
        REQUIRE(writer->AddStream(output_type.get(), &stream_index) == S_OK);
    }

    com_ptr<IMFMediaType> input_type{};
    {
        REQUIRE(MFCreateMediaType(input_type.put()) == S_OK);
        REQUIRE(input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video) == S_OK);
        REQUIRE(input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32) == S_OK);
        REQUIRE(input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive) == S_OK);
        REQUIRE(MFSetAttributeSize(input_type.get(), MF_MT_FRAME_SIZE, width, height) == S_OK);
        REQUIRE(MFSetAttributeRatio(input_type.get(), MF_MT_FRAME_RATE, fps, 1) == S_OK);
        REQUIRE(MFSetAttributeRatio(input_type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1) == S_OK);
        REQUIRE(writer->SetInputMediaType(stream_index, input_type.get(), NULL) == S_OK);
    }

    SECTION("Begin / Flush without WriteSample") {
        REQUIRE(writer->BeginWriting() == S_OK);
        REQUIRE(writer->Flush(stream_index) == S_OK);
    }
    // todo: SendStreamTick
    SECTION("WriteSample / Finalize") {
        REQUIRE(writer->BeginWriting() == S_OK);
        auto bufsz = 4 * width * height;
        CAPTURE(bufsz);

        for (LONGLONG time_point = 0; time_point < total_duration; time_point += frame_duration) {
            com_ptr<IMFSample> sample{};
            if (auto hr = create_single_buffer_sample(sample.put(), bufsz))
                FAIL(hr);

            com_ptr<IMFMediaBuffer> buffer{};
            if (auto hr = sample->GetBufferByIndex(0, buffer.put()))
                FAIL(hr);
            if (auto hr = buffer->SetCurrentLength(bufsz)) // 0 -> bufsz
                FAIL(hr);

            if (auto hr = sample->SetSampleTime(time_point))
                FAIL(hr);
            if (auto hr = sample->SetSampleDuration(frame_duration))
                FAIL(hr);
            if (auto hr = writer->WriteSample(stream_index, sample.get()))
                FAIL(hr);
        }
        REQUIRE(writer->Finalize() == S_OK);
    }
}

DWORD create_test_window(gsl::not_null<std::promise<HWND>*> p) {
    try {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"Window(IMFMediaSink)";

        auto const atom = RegisterClassW(&wc);
        if (atom == NULL) // probably ERROR_CLASS_ALREADY_EXISTS
            winrt::throw_last_error();
        auto on_return1 = gsl::finally([&wc]() { UnregisterClassW(wc.lpszClassName, wc.hInstance); });

        auto hwnd = CreateWindowExW(0, wc.lpszClassName, wc.lpszClassName, WS_OVERLAPPEDWINDOW, //
                                    CW_USEDEFAULT, CW_USEDEFAULT, 640, 360,                     //
                                    NULL, NULL, wc.hInstance, NULL);
        if (hwnd == NULL)
            winrt::throw_last_error();
        auto on_return2 = gsl::finally([hwnd]() { DestroyWindow(hwnd); });

        // note: `GetForegroundWindow` may not return this handle if you are debugging with Visual Studio
        if (SetForegroundWindow(hwnd) == false)
            winrt::throw_last_error();
        ShowWindow(hwnd, SW_SHOWDEFAULT);
        SetFocus(hwnd);
        p->set_value(hwnd);

        // serve messages in the background
        MSG msg{};
        while (msg.message != WM_QUIT) {
            msg = MSG{};
            if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE) == false) {
                SleepEx(10, true);
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return msg.wParam;
    } catch (...) {
        p->set_exception(std::current_exception());
    }
}

TEST_CASE("Window with Win32 API", "[window]") {
    std::promise<HWND> hwnd_promise{};
    std::future<DWORD> background = std::async(std::launch::async, create_test_window, &hwnd_promise);

    HWND hwnd = hwnd_promise.get_future().get();
    REQUIRE(hwnd != NULL);

    SECTION("Immediate close") {
        REQUIRE(PostMessageW(hwnd, WM_QUIT, NULL, NULL));
        REQUIRE(background.get() == S_OK);
    }
    SECTION("close via gsl::finally") {
        auto on_return = gsl::finally([hwnd, &background]() {
            REQUIRE(PostMessageW(hwnd, WM_QUIT, NULL, NULL));
            REQUIRE(background.get() == S_OK);
        });
    }
}

/// @see https://docs.microsoft.com/en-us/windows/win32/api/mfidl/nn-mfidl-imfmediatypehandler
com_ptr<IMFMediaType> create_test_sink_type(const RECT& region) {
    com_ptr<IMFMediaType> media_type{};
    REQUIRE(MFCreateMediaType(media_type.put()) == S_OK);
    REQUIRE(media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video) == S_OK);
    REQUIRE(media_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32) == S_OK);
    REQUIRE(media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive) == S_OK);
    REQUIRE(media_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE) == S_OK);
    REQUIRE(MFSetAttributeRatio(media_type.get(), MF_MT_PIXEL_ASPECT_RATIO, 16, 9) == S_OK);              // 16, 9
    REQUIRE(MFSetAttributeSize(media_type.get(), MF_MT_FRAME_SIZE, region.right, region.bottom) == S_OK); // 640, 360
    REQUIRE(MFSetAttributeRatio(media_type.get(), MF_MT_FRAME_RATE, 30, 1) == S_OK);
    return media_type;
}

// @see https://github.com/sipsorcery/mediafoundationsamples - MFVideoEVR/MFVideoEVR.cpp
TEST_CASE("IMFMediaSink(MFVideoEVR) - IMFVideoSampleAllocator", "[window]") {
    std::promise<HWND> hwnd_promise{};
    std::future<DWORD> background = std::async(std::launch::async, create_test_window, &hwnd_promise);
    HWND window = hwnd_promise.get_future().get();
    REQUIRE(window != NULL);
    auto on_return1 = gsl::finally([window, &background]() {
        PostMessageW(window, WM_QUIT, NULL, NULL);
        background.get();
    });

    auto on_return2 = media_startup();

    com_ptr<IMFActivate> activate{};
    REQUIRE(MFCreateVideoRendererActivate(window, activate.put()) == S_OK);
    com_ptr<IMFMediaSink> sink{};
    REQUIRE(activate->ActivateObject(IID_IMFMediaSink, (void**)sink.put()) == S_OK);

    com_ptr<IMFVideoRenderer> renderer{};
    REQUIRE(sink->QueryInterface(renderer.put()) == S_OK);
    com_ptr<IMFTransform> mixer{};
    com_ptr<IMFVideoPresenter> presenter{};
    REQUIRE(renderer->InitializeRenderer(mixer.get(), presenter.get()) == S_OK);
    com_ptr<IMFGetService> service{};
    REQUIRE(sink->QueryInterface(service.put()) == S_OK);

    com_ptr<IMFVideoDisplayControl> control{};
    REQUIRE(service->GetService(MR_VIDEO_RENDER_SERVICE, __uuidof(IMFVideoDisplayControl), (void**)control.put()) ==
            S_OK);
    REQUIRE(control->SetVideoWindow(window) == S_OK);
    RECT region{0, 0, 640, 360};
    REQUIRE(control->SetVideoPosition(nullptr, &region) == S_OK);

    DWORD num_stream_sink = 0;
    REQUIRE(sink->GetStreamSinkCount(&num_stream_sink) == S_OK);
    REQUIRE(num_stream_sink > 0);

    com_ptr<IMFStreamSink> stream_sink{};
    REQUIRE(sink->GetStreamSinkByIndex(num_stream_sink - 1, stream_sink.put()) == S_OK);
    com_ptr<IMFMediaTypeHandler> handler{};
    REQUIRE(stream_sink->GetMediaTypeHandler(handler.put()) == S_OK);

    com_ptr<IMFMediaType> sink_media_type = create_test_sink_type(region);
    REQUIRE(handler->SetCurrentMediaType(sink_media_type.get()) == S_OK);

    SECTION("without Direct3D") {
        com_ptr<IMFVideoSampleAllocator> allocator{};
        REQUIRE(MFGetService(stream_sink.get(), MR_VIDEO_ACCELERATION_SERVICE, IID_PPV_ARGS(allocator.put())) == S_OK);

        DWORD requested_frame_count = 5;
        REQUIRE(allocator->InitializeSampleAllocator(requested_frame_count, sink_media_type.get()) == S_OK);
        SECTION("allocate sample once") {
            com_ptr<IMFSample> sample{};
            REQUIRE(allocator->AllocateSample(sample.put()) == S_OK);
            com_ptr<IMFMediaBuffer> buffer{};
            REQUIRE(sample->GetBufferByIndex(0, buffer.put()) == S_OK);
            DWORD num_buffer = 0;
            REQUIRE(sample->GetBufferCount(&num_buffer) == S_OK);
            REQUIRE(num_buffer == 1);
        }
        SECTION("MF_E_SAMPLEALLOCATOR_EMPTY") {
            std::vector<com_ptr<IMFSample>> samples{};
            auto repeat = requested_frame_count;
            while (repeat--) {
                com_ptr<IMFSample> sample{};
                REQUIRE(allocator->AllocateSample(sample.put()) == S_OK);
                samples.emplace_back(std::move(sample));
            }
            REQUIRE(samples.size() == requested_frame_count);
            com_ptr<IMFSample> sample{};
            REQUIRE(allocator->AllocateSample(sample.put()) == MF_E_SAMPLEALLOCATOR_EMPTY);
        }
    }
    // see https://docs.microsoft.com/en-us/windows/win32/medfound/direct3d-device-manager
    SECTION("Direct3D Device Manager") {
        com_ptr<IDirect3DDeviceManager9> device_manager{};
        REQUIRE(MFGetService(sink.get(), MR_VIDEO_ACCELERATION_SERVICE, IID_PPV_ARGS(device_manager.put())) == S_OK);
        com_ptr<IMFVideoSampleAllocator> allocator{};
        REQUIRE(MFGetService(stream_sink.get(), MR_VIDEO_ACCELERATION_SERVICE, IID_PPV_ARGS(allocator.put())) == S_OK);
        REQUIRE(allocator->SetDirectXManager(device_manager.get()) == S_OK);

        DWORD requested_frame_count = 5;
        REQUIRE(allocator->InitializeSampleAllocator(requested_frame_count, sink_media_type.get()) == S_OK);
        SECTION("allocate sample once") {
            com_ptr<IMFSample> sample{};
            REQUIRE(allocator->AllocateSample(sample.put()) == S_OK);
            com_ptr<IMFMediaBuffer> buffer{};
            REQUIRE(sample->GetBufferByIndex(0, buffer.put()) == S_OK);
            DWORD num_buffer = 0;
            REQUIRE(sample->GetBufferCount(&num_buffer) == S_OK);
            REQUIRE(num_buffer == 1);
        }
        SECTION("MF_E_SAMPLEALLOCATOR_EMPTY") {
            std::vector<com_ptr<IMFSample>> samples{};
            auto repeat = requested_frame_count;
            while (repeat--) {
                com_ptr<IMFSample> sample{};
                REQUIRE(allocator->AllocateSample(sample.put()) == S_OK);
                samples.emplace_back(std::move(sample));
            }
            REQUIRE(samples.size() == requested_frame_count);
            com_ptr<IMFSample> sample{};
            REQUIRE(allocator->AllocateSample(sample.put()) == MF_E_SAMPLEALLOCATOR_EMPTY);
        }
    }
}

TEST_CASE("IMFMediaSink(MFVideoEVR) - Clock", "[window]") {
    std::promise<HWND> hwnd_promise{};
    std::future<DWORD> background = std::async(std::launch::async, create_test_window, &hwnd_promise);
    HWND window = hwnd_promise.get_future().get();
    REQUIRE(window != NULL);
    auto on_return1 = gsl::finally([window, &background]() {
        PostMessageW(window, WM_QUIT, NULL, NULL);
        background.get();
    });
    auto on_return2 = media_startup();

    com_ptr<IMFActivate> activate{};
    REQUIRE(MFCreateVideoRendererActivate(window, activate.put()) == S_OK);
    com_ptr<IMFMediaSink> sink{};
    REQUIRE(activate->ActivateObject(IID_IMFMediaSink, (void**)sink.put()) == S_OK);
    com_ptr<IMFVideoRenderer> renderer{};
    REQUIRE(sink->QueryInterface(renderer.put()) == S_OK);
    REQUIRE(renderer->InitializeRenderer(nullptr, nullptr) == S_OK);
    com_ptr<IMFGetService> service{};
    REQUIRE(sink->QueryInterface(service.put()) == S_OK);

    com_ptr<IMFVideoDisplayControl> control{};
    REQUIRE(service->GetService(MR_VIDEO_RENDER_SERVICE, __uuidof(IMFVideoDisplayControl), (void**)control.put()) ==
            S_OK);
    REQUIRE(control->SetVideoWindow(window) == S_OK);
    RECT region{0, 0, 640, 360};
    REQUIRE(control->SetVideoPosition(nullptr, &region) == S_OK);

    com_ptr<IMFStreamSink> stream_sink{};
    REQUIRE(sink->GetStreamSinkByIndex(0, stream_sink.put()) == S_OK);
    com_ptr<IMFMediaTypeHandler> handler{};
    REQUIRE(stream_sink->GetMediaTypeHandler(handler.put()) == S_OK);
    com_ptr<IMFMediaType> sink_media_type = create_test_sink_type(region);
    REQUIRE(handler->SetCurrentMediaType(sink_media_type.get()) == S_OK);

    com_ptr<IDirect3DDeviceManager9> device_manager{};
    REQUIRE(MFGetService(sink.get(), MR_VIDEO_ACCELERATION_SERVICE, IID_PPV_ARGS(device_manager.put())) == S_OK);
    com_ptr<IMFVideoSampleAllocator> allocator{};
    REQUIRE(MFGetService(stream_sink.get(), MR_VIDEO_ACCELERATION_SERVICE, IID_PPV_ARGS(allocator.put())) == S_OK);
    REQUIRE(allocator->SetDirectXManager(device_manager.get()) == S_OK);

    com_ptr<IMFPresentationClock> clock{};
    REQUIRE(MFCreatePresentationClock(clock.put()) == S_OK);
    com_ptr<IMFPresentationTimeSource> time_source{};
    REQUIRE(MFCreateSystemTimeSource(time_source.put()) == S_OK);
    REQUIRE(clock->SetTimeSource(time_source.get()) == S_OK);
    REQUIRE(sink->SetPresentationClock(clock.get()) == S_OK);

    DWORD requested_frame_count = 5;
    REQUIRE(allocator->InitializeSampleAllocator(requested_frame_count, sink_media_type.get()) == S_OK);

    LONGLONG time_stamp = 0;
    REQUIRE(clock->Start(time_stamp) == S_OK);
    DWORD repeat = 300;
    while (repeat--) {
        com_ptr<IMFSample> sample{};
        if (auto ec = allocator->AllocateSample(sample.put()))
            break;

        constexpr auto duration = 1000u;
        time_stamp += duration;
        sample->SetSampleTime(time_stamp);
        sample->SetSampleDuration(duration);

        if (auto ec = stream_sink->ProcessSample(sample.get()))
            FAIL(ec);
    }
    REQUIRE(clock->Stop() == S_OK);
}
