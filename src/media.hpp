#pragma once
// #include <winrt/Windows.Foundation.h>
// #include <winrt/Windows.System.Threading.h>

#include <experimental/generator>
#include <filesystem>
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
namespace fs = std::filesystem;

/// @see MFStartup
/// @see MFShutdown
/// @throw winrt::hresult_error
auto media_startup() noexcept(false) -> gsl::final_action<HRESULT(WINAPI*)()>;

/// @see MFEnumDeviceSources
HRESULT get_devices(gsl::not_null<IMFAttributes*> attrs, std::vector<ComPtr<IMFActivate>>& devices) noexcept;

HRESULT get_name(gsl::not_null<IMFActivate*> device, std::wstring& name) noexcept;
HRESULT get_name(gsl::not_null<IMFActivate*> device, std::string& name) noexcept;

/// @see MFCreateSourceResolver
HRESULT resolve(const fs::path& fpath, IMFMediaSourceEx** source, MF_OBJECT_TYPE& media_object_type) noexcept;

/// @see CoCreateInstance
/// @see CLSID_CMSH264DecoderMFT
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/h-264-video-decoder
HRESULT make_transform_H264(IMFTransform** transform) noexcept;

/// @see CoCreateInstance
/// @see Color Converter DSP https://docs.microsoft.com/en-us/windows/win32/medfound/colorconverter
/// @param iid CLSID_CColorConvertDMO
HRESULT make_transform_video(IMFTransform** transform, const IID& iid) noexcept;

/// @see CoCreateInstance
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/video-processor-mft
HRESULT make_transform_video(IMFTransform** transform) noexcept;

/**
 * @todo: configure MF_MT_FRAME_SIZE, MF_MT_FRAME_RATE
 * @todo: configure MF_MT_PIXEL_ASPECT_RATIO
 */
HRESULT make_video_output_RGB32(IMFMediaType** ptr) noexcept;

HRESULT make_video_output_RGB565(IMFMediaType** ptr) noexcept;


HRESULT try_output_type(IMFTransform* transform, DWORD ostream, const GUID& desired,
                        IMFMediaType** output_type) noexcept;


auto get_input_available_types(ComPtr<IMFTransform> transform, DWORD num_input, HRESULT& ec) noexcept(false)
    -> std::experimental::generator<ComPtr<IMFMediaType>>;
auto try_output_available_types(ComPtr<IMFTransform> transform, DWORD stream_id, DWORD& type_index) noexcept(false)
    -> std::experimental::generator<ComPtr<IMFMediaType>>;

auto get_output_available_types(ComPtr<IMFTransform> transform, DWORD num_output, HRESULT& ec) noexcept(false)
    -> std::experimental::generator<ComPtr<IMFMediaType>>;
auto try_input_available_types(ComPtr<IMFTransform> transform, DWORD stream_id, DWORD& type_index) noexcept(false)
    -> std::experimental::generator<ComPtr<IMFMediaType>>;

auto read_samples(ComPtr<IMFSourceReader> source_reader, //
                  DWORD& index, DWORD& flags, LONGLONG& timestamp, LONGLONG& duration) noexcept(false)
    -> std::experimental::generator<ComPtr<IMFSample>>;

auto decode(ComPtr<IMFTransform> transform, ComPtr<IMFMediaType> output_type, DWORD ostream_id) noexcept(false)
    -> std::experimental::generator<ComPtr<IMFSample>>;

auto process(ComPtr<IMFTransform> transform, DWORD istream, DWORD ostream,     //
             ComPtr<IMFSample> input_sample, ComPtr<IMFMediaType> output_type, //
             HRESULT& hr) noexcept -> std::experimental::generator<ComPtr<IMFSample>>;
auto process(ComPtr<IMFTransform> transform, DWORD istream, DWORD ostream, //
             ComPtr<IMFSourceReader> source_reader,                        //
             HRESULT& ec) -> std::experimental::generator<ComPtr<IMFSample>>;

HRESULT create_single_buffer_sample(DWORD bufsz, IMFSample** sample);
HRESULT create_and_copy_single_buffer_sample(IMFSample* src, IMFSample** dst);
HRESULT get_transform_output(IMFTransform* transform, IMFSample** sample, BOOL& flushed);

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

std::string to_readable(const GUID& guid) noexcept;

void print(gsl::not_null<IMFActivate*> device) noexcept;
void print(gsl::not_null<IMFTransform*> transform) noexcept;
void print(gsl::not_null<IMFMediaType*> media) noexcept;
