/**
 * @file    media.hpp
 * @author  github.com/luncliff (luncliff@gmail.com)
 * @brief   pch.h for this project
 */
#pragma once
#include <experimental/generator>
#include <filesystem>
#include <future>
#include <gsl/gsl>

#include <pplawait.h>
#include <ppltasks.h>
#include <winrt/Windows.Foundation.h> // namespace winrt::Windows::Foundation
#include <winrt/Windows.System.h>     // namespace winrt::Windows::System

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wmcodecdsp.h>

// C++ 17 Coroutines TS
using std::experimental::coroutine_handle;
using std::experimental::generator;
// replaces Microsoft::WRL::ComPtr. see https://docs.microsoft.com/en-us/windows/uwp/cpp-and-winrt-apis/move-to-winrt-from-wrl
using winrt::com_ptr;

namespace fs = std::filesystem;

/// @see MFStartup
/// @see MFShutdown
/// @throw winrt::hresult_error
auto media_startup() noexcept(false) -> gsl::final_action<HRESULT(WINAPI*)()>;

/// @see MFEnumDeviceSources
HRESULT get_devices(std::vector<com_ptr<IMFActivate>>& devices, IMFAttributes* attributes) noexcept;

/// @see https://docs.microsoft.com/en-us/windows/win32/api/mfobjects/nf-mfobjects-imfattributes-getstring
HRESULT get_string(gsl::not_null<IMFAttributes*> attribute, const GUID& uuid, winrt::hstring& name) noexcept;

/// @see MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME
HRESULT get_name(gsl::not_null<IMFActivate*> device, winrt::hstring& name) noexcept;
[[deprecated]] HRESULT get_name(gsl::not_null<IMFActivate*> device, std::wstring& name) noexcept;
[[deprecated]] HRESULT get_name(gsl::not_null<IMFActivate*> device, std::string& name) noexcept;

HRESULT get_hardware_url(gsl::not_null<IMFTransform*> transform, winrt::hstring& name) noexcept;

/// @see MFCreateSourceResolver
HRESULT resolve(const fs::path& fpath, IMFMediaSourceEx** source, MF_OBJECT_TYPE& media_object_type) noexcept;

/// @see CoCreateInstance
/// @see Color Converter DSP https://docs.microsoft.com/en-us/windows/win32/medfound/colorconverter
/// @see H264 Decoder https://docs.microsoft.com/en-us/windows/win32/medfound/h-264-video-decoder
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/video-processor-mft
/// @param iid CLSID_CColorConvertDMO, CLSID_CMSH264DecoderMFT, CLSID_VideoProcessorMFT
/// @todo test enCLMFTDx11
HRESULT make_transform_video(IMFTransform** transform, const IID& iid) noexcept;

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/h-264-video-decoder#transform-attributes
HRESULT configure_acceleration_H264(gsl::not_null<IMFTransform*> transform) noexcept;

/// @brief configure D3D11 if the transform supports it
/// @return E_NOTIMPL, E_FAIL ...
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/hardware-mfts
HRESULT configure_D3D11_DXGI(gsl::not_null<IMFTransform*> transform,
                             gsl::not_null<IMFDXGIDeviceManager*> device_manager) noexcept;

/// @brief exactly same sized src/dst rectangle;
HRESULT configure_rectangle(gsl::not_null<IMFVideoProcessorControl*> control,
                            gsl::not_null<IMFMediaType*> media_type) noexcept;

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/videoresizer
HRESULT configure_source_rectangle(gsl::not_null<IPropertyStore*> props, const RECT& rect) noexcept;

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/videoresizer
HRESULT configure_destination_rectangle(gsl::not_null<IPropertyStore*> props, const RECT& rect) noexcept;

HRESULT make_video_RGB32(gsl::not_null<IMFMediaType**> ptr) noexcept;
HRESULT make_video_RGB565(gsl::not_null<IMFMediaType**> ptr) noexcept;
HRESULT make_video_type(gsl::not_null<IMFMediaType**> ptr, const GUID& subtype) noexcept;

HRESULT try_output_type(com_ptr<IMFTransform> transform, DWORD ostream, const GUID& desired,
                        IMFMediaType** output_type) noexcept;

auto get_input_available_types(com_ptr<IMFTransform> transform, DWORD num_input, HRESULT& ec) noexcept(false)
    -> generator<com_ptr<IMFMediaType>>;

auto try_output_available_types(com_ptr<IMFTransform> transform, DWORD stream_id, DWORD& type_index) noexcept(false)
    -> generator<com_ptr<IMFMediaType>>;

auto get_output_available_types(com_ptr<IMFTransform> transform, DWORD num_output, HRESULT& ec) noexcept(false)
    -> generator<com_ptr<IMFMediaType>>;
auto try_input_available_types(com_ptr<IMFTransform> transform, DWORD stream_id, DWORD& type_index) noexcept(false)
    -> generator<com_ptr<IMFMediaType>>;

/**
 * @brief Read `IMFSample`s from `MF_SOURCE_READER_FIRST_VIDEO_STREAM` of the given `IMFSourceReader`
 * @todo https://docs.microsoft.com/en-us/windows/win32/medfound/mf-source-reader-enable-advanced-video-processing#remarks
 * 
 * @note `co_yield`ed `IMFSample`s' timestamp values are already updated in the routine
 */
auto read_samples(com_ptr<IMFSourceReader> source_reader, //
                  DWORD& stream_index, DWORD& flags, LONGLONG& timestamp) noexcept(false)
    -> generator<com_ptr<IMFSample>>;

auto decode(com_ptr<IMFTransform> transform, DWORD ostream, com_ptr<IMFMediaType> output_type, //
            HRESULT& ec) noexcept -> generator<com_ptr<IMFSample>>;

auto process(com_ptr<IMFTransform> transform, DWORD istream, DWORD ostream,      //
             com_ptr<IMFSample> input_sample, com_ptr<IMFMediaType> output_type, //
             HRESULT& ec) noexcept -> generator<com_ptr<IMFSample>>;
auto process(com_ptr<IMFTransform> transform, DWORD istream, DWORD ostream, //
             com_ptr<IMFSourceReader> source_reader,                        //
             HRESULT& ec) -> generator<com_ptr<IMFSample>>;

HRESULT create_single_buffer_sample(IMFSample** sample, DWORD bufsz);
HRESULT create_and_copy_single_buffer_sample(IMFSample* src, IMFSample** dst);
HRESULT get_transform_output(IMFTransform* transform, IMFSample** sample, BOOL& flushed);

/**
 * @note `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS` == `TRUE`
 * @note `MF_SINK_WRITER_DISABLE_THROTTLING` == `FALSE`
 */
HRESULT create_sink_writer(IMFSinkWriterEx** writer, const fs::path& fpath) noexcept;

/// @todo mock `CoCreateInstance`
HRESULT create_reader_callback(IMFSourceReaderCallback** callback) noexcept;

/**
 * @note `MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING` == `TRUE`
 * @note `MF_READWRITE_DISABLE_CONVERTERS` == `FALSE`
 * 
 * @param callback  if not `nullptr`, redirected to `MF_SOURCE_READER_ASYNC_CALLBACK`
 * @return HRESULT  from `MFCreateSourceReaderFromMediaSource`
 */
HRESULT create_source_reader(com_ptr<IMFMediaSource> source, com_ptr<IMFSourceReaderCallback> callback,
                             IMFSourceReader** reader) noexcept;

/**
 * @brief Get the first available `IMFStreamDescriptor`
 * 
 * @param ptr   Will be reset to `nullptr` if there is no available stream descriptor
 */
HRESULT get_stream_descriptor(gsl::not_null<IMFPresentationDescriptor*> presentation,
                              IMFStreamDescriptor** ptr) noexcept;

/**
 * @see https://docs.microsoft.com/en-us/windows/win32/medfound/video-processor-mft
 * @see https://docs.microsoft.com/en-us/windows/win32/medfound/video-media-types
 * @see https://docs.microsoft.com/en-us/windows/win32/medfound/about-yuv-video
 */
HRESULT configure(com_ptr<IMFStreamDescriptor> stream) noexcept;

std::string to_string(const GUID& guid) noexcept;
std::string to_readable(const GUID& guid) noexcept;
std::string to_readable(HRESULT hr) noexcept;

winrt::hstring to_hstring(const GUID& guid) noexcept;

void print_error(winrt::hresult code, std::string&& message) noexcept;
void print_error(const winrt::hresult_error& ex) noexcept;

/**
 * @brief print description for the `media_type` with logging
 * @note the argument is considered immutable
 */
void print(gsl::not_null<IMFActivate*> device) noexcept;

/**
 * @brief print description for the `media_type` with logging
 * @note the argument is considered immutable
 */
void print(gsl::not_null<IMFMediaType*> media_type) noexcept;

/**
 * @brief print description for the `media_type` with logging
 * @note the function may change modify input/output configuration
 */
void print(gsl::not_null<IMFTransform*> transform, const GUID& iid) noexcept;

class h264_video_writer_t final {
    com_ptr<IMFSinkWriterEx> writer{}; // expose IMFTransform for each stream
    com_ptr<IMFMediaType> output_type{};
    DWORD stream_index = 0;

  public:
    explicit h264_video_writer_t(const fs::path& fpath) noexcept(false);
    ~h264_video_writer_t() noexcept;
    h264_video_writer_t(const h264_video_writer_t&) = delete;
    h264_video_writer_t(h264_video_writer_t&&) = delete;
    h264_video_writer_t& operator=(const h264_video_writer_t&) = delete;
    h264_video_writer_t& operator=(h264_video_writer_t&&) = delete;

    HRESULT use_source(com_ptr<IMFMediaType> input_type) noexcept;
    HRESULT begin() noexcept;
    HRESULT write(IMFSample* sample) noexcept;
};

class capture_session_t final {
    com_ptr<IMFMediaSourceEx> source{}; // supports D3DManager
    com_ptr<IMFMediaType> source_type{};
    com_ptr<IMFSourceReaderEx> reader{}; // expose IMFTransform for each stream
    com_ptr<IMFSourceReaderCallback> reader_callback{};
    DWORD reader_stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

  public:
    explicit capture_session_t(com_ptr<IMFActivate> device) noexcept(false);
    ~capture_session_t() noexcept;
    capture_session_t(const capture_session_t&) = delete;
    capture_session_t(capture_session_t&&) = delete;
    capture_session_t& operator=(const capture_session_t&) = delete;
    capture_session_t& operator=(capture_session_t&&) = delete;

    HRESULT open(com_ptr<IMFSourceReaderCallback> callback, DWORD stream) noexcept;

    com_ptr<IMFSourceReader> get_reader() const noexcept;
    com_ptr<IMFMediaType> get_source_type() const noexcept;
};
