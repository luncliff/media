#pragma once
#include <chrono>
#include <ctime>
#include <gsl/gsl>

#include <comdef.h>
#include <shlwapi.h>
#include <wrl/client.h>
//#include <mferror.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

using Microsoft::WRL::ComPtr;

auto startup() -> gsl::final_action<HRESULT(WINAPI*)()>;

HRESULT get_devices(IMFAttributes* attrs, std::vector<ComPtr<IMFActivate>>& devices) noexcept;
HRESULT get_name(IMFActivate* device, std::wstring& name) noexcept;

HRESULT get_stream_descriptor(IMFPresentationDescriptor* presentation, IMFStreamDescriptor** ptr);

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/video-processor-mft
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/video-media-types
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/about-yuv-video
HRESULT configure(ComPtr<IMFStreamDescriptor> stream) noexcept;

class device_group_t final {
  public:
    ComPtr<IMFAttributes> attrs{};
    std::vector<ComPtr<IMFActivate>> devices{};

  public:
    /// @throw _com_error
    device_group_t() noexcept(false);
};

/// @see OnReadSample
struct read_item_t final {
    HRESULT status;
    DWORD index;
    DWORD flags;
    std::chrono::nanoseconds timestamp; // unit: 100 nanosecond
    ComPtr<IMFSample> sample;
};
static_assert(sizeof(read_item_t) == 32);

/// @see clock_gettime, CLOCKS_PER_SEC
class process_timer_t final {
    clock_t start = clock();

  public:
    /**
     * @return float elapsed second
     */
    float pick() const noexcept {
        const auto now = clock();
        return static_cast<float>(now - start) / CLOCKS_PER_SEC;
    }
    float reset() noexcept {
        const auto d = this->pick();
        start = clock();
        return d;
    }
};
