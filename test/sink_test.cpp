/**
 * @file    sink_test.cpp
 * @author  github.com/luncliff (luncliff@gmail.com)
 * @see     https://docs.microsoft.com/en-us/windows/win32/medfound/sink-writer
 * @see     https://docs.microsoft.com/en-us/windows/win32/api/mfreadwrite/nn-mfreadwrite-imfsinkwriter
 */
#define CATCH_CONFIG_WINDOWS_CRTDBG
#include <catch2/catch.hpp>
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

// https://docs.microsoft.com/en-us/windows/win32/medfound/using-the-sink-writer
// https://docs.microsoft.com/en-us/windows/win32/medfound/tutorial--using-the-sink-writer-to-encode-video#define-the-video-format
TEST_CASE("IMFMediaSink(MPEG4)", "[window][!mayfail]") {
    auto on_return = media_startup();

    // todo: MFCreateSinkWriterFromURL(L"output.mp4", NULL, NULL, sink_writer.put());
    com_ptr<IMFReadWriteClassFactory> factory{};
    REQUIRE(CoCreateInstance(CLSID_MFReadWriteClassFactory, NULL, CLSCTX_INPROC_SERVER, //
                             IID_PPV_ARGS(factory.put())) == S_OK);

    com_ptr<IMFSinkWriterEx> sink_writer{};
    {
        com_ptr<IMFAttributes> attrs{};
        REQUIRE(MFCreateAttributes(attrs.put(), 1) == S_OK);
        REQUIRE(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE) == S_OK);

        com_ptr<IMFSinkWriter> writer{};
        const auto fpath = fs::current_path() / L"output.mp4";
        REQUIRE(factory->CreateInstanceFromURL(CLSID_MFSinkWriter, fpath.c_str(), attrs.get(), //
                                               IID_PPV_ARGS(writer.put())) == S_OK);
        REQUIRE(writer->QueryInterface(sink_writer.put()) == S_OK);
    }

    SECTION("MF_SINK_WRITER_STATISTICS") {
        MF_SINK_WRITER_STATISTICS stats{};
        REQUIRE(sink_writer->GetStatistics(0, &stats) == S_OK);
    }
    SECTION("IMFMediaSink") {
        com_ptr<IMFMediaType> video_type{};
        REQUIRE(video_type);
        REQUIRE(video_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video) == S_OK);
        REQUIRE(video_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264) == S_OK);
        REQUIRE(video_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive) == S_OK);
        REQUIRE(video_type->SetUINT32(MF_MT_AVG_BITRATE, 800'000) == S_OK);
        REQUIRE(MFSetAttributeRatio(video_type.get(), MF_MT_FRAME_RATE, 30, 1) == S_OK); // 30 fps

        // ... The sink writer supports the following combinations: Uncompressed input with compressed output ...
        // ... Uncompressed input with identical output ...
        com_ptr<IMFByteStream> byte_stream{};
        com_ptr<IMFMediaType> audio_type{};
        com_ptr<IMFMediaSink> sink{};
        REQUIRE(MFCreateMPEG4MediaSink(byte_stream.get(), video_type.get(), audio_type.get(), sink.put()) == S_OK);

        // https://docs.microsoft.com/en-us/windows/win32/medfound/tutorial--using-the-sink-writer-to-encode-video#send-video-frames-to-the-sink-writer
        com_ptr<IMFSample> sample{};
        REQUIRE(sink_writer->WriteSample(0, sample.get()) == S_OK);
    }
}

// @see https://github.com/sipsorcery/mediafoundationsamples - MFVideoEVR/MFVideoEVR.cpp
TEST_CASE("IMFMediaSink(MFVideoEVR)", "[window][!mayfail]") {
    HWND window = NULL;
    auto on_return = media_startup();

    com_ptr<IMFActivate> activate{};
    REQUIRE(MFCreateVideoRendererActivate(window, activate.put()) == S_OK);
    com_ptr<IMFMediaSink> sink{};
    REQUIRE(activate->ActivateObject(IID_IMFMediaSink, (void**)sink.put()) == S_OK);

    com_ptr<IMFVideoRenderer> renderer{};
    REQUIRE(sink->QueryInterface(renderer.put()) == S_OK);
    {
        IMFTransform* mixer = nullptr;
        IMFVideoPresenter* presenter = nullptr;
        REQUIRE(renderer->InitializeRenderer(mixer, presenter) == S_OK);
    }
    com_ptr<IMFGetService> service{};
    REQUIRE(sink->QueryInterface(service.put()) == S_OK);

    com_ptr<IMFVideoDisplayControl> control{};
    REQUIRE(service->GetService(MR_VIDEO_RENDER_SERVICE, __uuidof(IMFVideoDisplayControl), (void**)control.put()) ==
            S_OK);
    {
        REQUIRE(control->SetVideoWindow(window) == S_OK);
        RECT region{0, 0, 640, 480};
        REQUIRE(control->SetVideoPosition(nullptr, &region) == S_OK);
    }

    SECTION("IMFStreamSink") {
        com_ptr<IMFStreamSink> stream_sink{};
        REQUIRE(sink->GetStreamSinkByIndex(0, stream_sink.put()) == S_OK);
        com_ptr<IMFMediaTypeHandler> handler{};
        REQUIRE(stream_sink->GetMediaTypeHandler(handler.put()) == S_OK);

        // see https://docs.microsoft.com/en-us/windows/win32/api/mfidl/nn-mfidl-imfmediatypehandler
        SECTION("IMFMediaTypeHandler") {
            GUID major{};
            REQUIRE(handler->GetMajorType(&major) == S_OK);
            REQUIRE(major == MFMediaType_Video);

            DWORD num_types = 0;
            REQUIRE(handler->GetMediaTypeCount(&num_types) == S_OK);
            REQUIRE(num_types > 0);
            for (auto i = 0u; i < num_types; ++i) {
                com_ptr<IMFMediaType> media_type{};
                if (auto hr = handler->GetMediaTypeByIndex(i, media_type.put()))
                    FAIL(hr);
                print(media_type.get());
            }
        }

        // see https://docs.microsoft.com/en-us/windows/win32/medfound/direct3d-device-manager
        SECTION("Direct3D Device Manager") {
            com_ptr<IMFVideoSampleAllocator> allocator{};
            REQUIRE(MFGetService(stream_sink.get(), MR_VIDEO_ACCELERATION_SERVICE, IID_PPV_ARGS(allocator.put())) ==
                    S_OK);
            com_ptr<IDirect3DDeviceManager9> device_manager{};
            REQUIRE(MFGetService(sink.get(), MR_VIDEO_ACCELERATION_SERVICE, IID_PPV_ARGS(device_manager.put())) ==
                    S_OK);
            REQUIRE(allocator->SetDirectXManager(device_manager.get()) == S_OK);

            com_ptr<IMFSample> sample{};
            REQUIRE(allocator->AllocateSample(sample.put()) == S_OK);
            com_ptr<IMFMediaBuffer> buffer{};
            REQUIRE(sample->GetBufferByIndex(0, buffer.put()) == S_OK);
        }
    }
}

DWORD test_thread_procedure(std::atomic<HWND>& window) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"Test Window(IMFMediaSink)";

    auto const atom = RegisterClassW(&wc);
    if (atom == NULL) // probably ERROR_CLASS_ALREADY_EXISTS
        return GetLastError();
    auto on_return1 = gsl::finally([&wc]() {
        if (UnregisterClassW(wc.lpszClassName, wc.hInstance) == false)
            winrt::throw_last_error();
    });

    window = CreateWindowExW(0, wc.lpszClassName, wc.lpszClassName, WS_OVERLAPPEDWINDOW, //
                             CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,                     //
                             NULL, NULL, wc.hInstance, NULL);
    if (window == NULL)
        return GetLastError();
    auto on_return2 = gsl::finally([hwnd = window.load()]() { DestroyWindow(hwnd); });

    ShowWindow(window, SW_SHOWDEFAULT);
    MSG msg{};
    while (msg.message != WM_QUIT) {
        if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE) == false) {
            SleepEx(10, true);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return GetLastError();
}

class app_context_t final {
    HINSTANCE instance;
    HWND window;
    WNDCLASSEXW winclass;
    ATOM atom;

  public:
    static LRESULT CALLBACK on_key_msg(HWND hwnd, UINT umsg, //
                                       WPARAM wparam, LPARAM lparam) {
        switch (umsg) {
        case WM_KEYDOWN:
        case WM_KEYUP:
            return 0;
        default:
            return DefWindowProc(hwnd, umsg, wparam, lparam);
        }
    }

    static LRESULT CALLBACK on_wnd_msg(HWND hwnd, UINT umsg, //
                                       WPARAM wparam, LPARAM lparam) {
        switch (umsg) {
        case WM_DESTROY:
        case WM_CLOSE:
            PostQuitMessage(EXIT_SUCCESS); // == 0
            return 0;
        case WM_CREATE:
            lparam;
        default:
            return on_key_msg(hwnd, umsg, wparam, lparam);
        }
    }

  public:
    explicit app_context_t(LPWSTR name, LPVOID param) : instance{GetModuleHandle(NULL)}, window{}, winclass{}, atom{} {
        // Setup the windows class with default settings.
        winclass.cbSize = sizeof(WNDCLASSEXW);
        winclass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        winclass.lpfnWndProc = on_wnd_msg;
        winclass.hInstance = instance;
        winclass.hCursor = LoadCursor(NULL, IDC_ARROW);
        winclass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        winclass.lpszMenuName = NULL;
        winclass.lpszClassName = name;
        atom = RegisterClassExW(&winclass);
        if (atom == NULL)
            winrt::throw_last_error();

        int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        auto screenWidth = 100 * 16, screenHeight = 100 * 9;
        // fullscreen: maximum size of the users desktop and 32bit
        if (true) {
            DEVMODE mode{};
            mode.dmSize = sizeof(mode);
            mode.dmPelsWidth = GetSystemMetrics(SM_CXSCREEN);
            mode.dmPelsHeight = GetSystemMetrics(SM_CYSCREEN);
            mode.dmBitsPerPel = 32;
            mode.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;
            // Change the display settings to full screen.
            ChangeDisplaySettings(&mode, CDS_FULLSCREEN);
        }

        // Create the window with the screen settings and get the handle to it.
        window = CreateWindowExW(0, winclass.lpszClassName, winclass.lpszClassName, WS_OVERLAPPEDWINDOW, //
                                 x, y, 640, 480,                                                         //
                                 NULL, NULL, winclass.hInstance, param);

        // Bring the window up on the screen and set it as main focus.
        ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
        SetFocus(window);
        ShowCursor(true);
    }
    ~app_context_t() {
        DestroyWindow(window);
        UnregisterClassW(winclass.lpszClassName, nullptr);
    }

    DWORD run() noexcept(false) {
        auto consume_messages = [](MSG& msg) {
            if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            return msg.message == WM_QUIT;
        };

        MSG msg{};
        while (consume_messages(msg) == false) {
            // ...
        }
        return static_cast<DWORD>(msg.wParam);
    }
};
