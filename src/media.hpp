#pragma once
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.Threading.h>

#include <experimental/generator>
#include <gsl/gsl>

#include <comdef.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

/// @see MFStartup
/// @see MFShutdown
/// @throw winrt::hresult_error
auto media_startup() noexcept(false) -> gsl::final_action<HRESULT(WINAPI*)()>;

/// @see MFEnumDeviceSources
HRESULT get_devices(IMFAttributes* attrs, std::vector<ComPtr<IMFActivate>>& devices) noexcept;
HRESULT get_name(IMFActivate* device, std::wstring& name) noexcept;

gsl::czstring<> get_name(const GUID& guid) noexcept;

HRESULT get_stream_descriptor(IMFPresentationDescriptor* presentation, IMFStreamDescriptor** ptr);

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/video-processor-mft
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/video-media-types
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/about-yuv-video
HRESULT configure(ComPtr<IMFStreamDescriptor> stream) noexcept;

/// @todo use `static_assert` for Windows SDK
class qpc_timer_t final {
    LARGE_INTEGER start{}, const frequency{};

  public:
    qpc_timer_t() noexcept {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
    }

    /// @return elapsed time in millisecond unit
    auto pick() const noexcept {
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&end);
        const auto elapsed = end.QuadPart - start.QuadPart;
        return (elapsed * 1'000) / frequency.QuadPart;
    }

    auto reset() noexcept {
        auto d = pick();
        QueryPerformanceCounter(&start);
        return d;
    }
};

auto decode(ComPtr<IMFSourceReader> source_reader, ComPtr<IMFTransform> decoding_transform) noexcept(false)
    -> std::experimental::generator<ComPtr<IMFSample>>;

HRESULT create_single_buffer_sample(DWORD bufsz, IMFSample** sample);
HRESULT create_and_copy_single_buffer_sample(IMFSample* src, IMFSample** dst);
HRESULT get_transform_output(IMFTransform* transform, IMFSample** sample, BOOL& flushed);
