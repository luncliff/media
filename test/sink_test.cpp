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

// @see https://github.com/sipsorcery/mediafoundationsamples - MFVideoEVR/MFVideoEVR.cpp
TEST_CASE("IMFMediaSink(MFVideoEVR)", "[window][!mayfail]") {
    HWND window = NULL;
    auto on_return = media_startup();

    ComPtr<IMFActivate> activate{};
    REQUIRE(MFCreateVideoRendererActivate(window, activate.GetAddressOf()) == S_OK);
    ComPtr<IMFMediaSink> sink{};
    REQUIRE(activate->ActivateObject(IID_IMFMediaSink, (void**)sink.GetAddressOf()) == S_OK);

    ComPtr<IMFVideoRenderer> renderer{};
    REQUIRE(sink->QueryInterface(renderer.GetAddressOf()) == S_OK);
    {
        IMFTransform* mixer = nullptr;
        IMFVideoPresenter* presenter = nullptr;
        REQUIRE(renderer->InitializeRenderer(mixer, presenter) == S_OK);
    }
    ComPtr<IMFGetService> service{};
    REQUIRE(sink->QueryInterface(service.GetAddressOf()) == S_OK);

    ComPtr<IMFVideoDisplayControl> control{};
    REQUIRE(service->GetService(MR_VIDEO_RENDER_SERVICE, __uuidof(IMFVideoDisplayControl),
                                (void**)control.GetAddressOf()) == S_OK);
    {
        REQUIRE(control->SetVideoWindow(window) == S_OK);
        RECT region{0, 0, 640, 480};
        REQUIRE(control->SetVideoPosition(nullptr, &region) == S_OK);
    }

    SECTION("IMFStreamSink") {
        ComPtr<IMFStreamSink> stream_sink{};
        REQUIRE(sink->GetStreamSinkByIndex(0, stream_sink.GetAddressOf()) == S_OK);
        ComPtr<IMFMediaTypeHandler> handler{};
        REQUIRE(stream_sink->GetMediaTypeHandler(handler.GetAddressOf()) == S_OK);

        // see https://docs.microsoft.com/en-us/windows/win32/api/mfidl/nn-mfidl-imfmediatypehandler
        SECTION("IMFMediaTypeHandler") {
            GUID major{};
            REQUIRE(handler->GetMajorType(&major) == S_OK);
            REQUIRE(major == MFMediaType_Video);

            DWORD num_types = 0;
            REQUIRE(handler->GetMediaTypeCount(&num_types) == S_OK);
            REQUIRE(num_types > 0);
            for (auto i = 0u; i < num_types; ++i) {
                ComPtr<IMFMediaType> media_type{};
                if (auto hr = handler->GetMediaTypeByIndex(i, media_type.GetAddressOf()))
                    FAIL(hr);
                print(media_type.Get());
            }
        }

        // see https://docs.microsoft.com/en-us/windows/win32/medfound/direct3d-device-manager
        SECTION("Direct3D Device Manager") {
            ComPtr<IMFVideoSampleAllocator> allocator{};
            REQUIRE(MFGetService(stream_sink.Get(), MR_VIDEO_ACCELERATION_SERVICE,
                                 IID_PPV_ARGS(allocator.GetAddressOf())) == S_OK);
            ComPtr<IDirect3DDeviceManager9> device_manager{};
            REQUIRE(MFGetService(sink.Get(), MR_VIDEO_ACCELERATION_SERVICE,
                                 IID_PPV_ARGS(device_manager.GetAddressOf())) == S_OK);
            REQUIRE(allocator->SetDirectXManager(device_manager.Get()) == S_OK);

            ComPtr<IMFSample> sample{};
            REQUIRE(allocator->AllocateSample(sample.GetAddressOf()) == S_OK);
            ComPtr<IMFMediaBuffer> buffer{};
            REQUIRE(sample->GetBufferByIndex(0, buffer.GetAddressOf()) == S_OK);
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
        return msg.wParam;
    }
};
