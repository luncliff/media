#pragma once
#include <winrt/base.h>

#include <experimental/generator>
#include <filesystem>
#include <future>
#include <gsl/gsl>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wmcodecdsp.h>

// C++ 17 Coroutines TS
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
HRESULT configure_D3D11_DXGI(gsl::not_null<IMFTransform*> transform, IMFDXGIDeviceManager* device_manager) noexcept;

/// @brief exactly same sized src/dst rectangle;
HRESULT configure_rectangle(gsl::not_null<IMFVideoProcessorControl*> control,
                            gsl::not_null<IMFMediaType*> media_type) noexcept;

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/videoresizer
HRESULT configure_source_rectangle(gsl::not_null<IPropertyStore*> props, const RECT& rect) noexcept;

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/videoresizer
HRESULT configure_destination_rectangle(gsl::not_null<IPropertyStore*> props, const RECT& rect) noexcept;

/**
 * @todo: configure MF_MT_FRAME_SIZE, MF_MT_FRAME_RATE
 * @todo: configure MF_MT_PIXEL_ASPECT_RATIO
 */
HRESULT make_video_output_RGB32(IMFMediaType** ptr) noexcept;

HRESULT make_video_output_RGB565(IMFMediaType** ptr) noexcept;

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

/// @todo https://docs.microsoft.com/en-us/windows/win32/medfound/mf-source-reader-enable-advanced-video-processing#remarks
auto read_samples(com_ptr<IMFSourceReader> source_reader, //
                  DWORD& index, DWORD& flags, LONGLONG& timestamp, LONGLONG& duration) noexcept(false)
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

/// @todo mock `CoCreateInstance`
HRESULT create_reader_callback(IMFSourceReaderCallback** callback) noexcept;

HRESULT create_source_reader(com_ptr<IMFMediaSource> source, com_ptr<IMFSourceReaderCallback> callback,
                             IMFSourceReader** reader) noexcept;

HRESULT get_stream_descriptor(IMFPresentationDescriptor* presentation, IMFStreamDescriptor** ptr) noexcept;

/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/video-processor-mft
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/video-media-types
/// @see https://docs.microsoft.com/en-us/windows/win32/medfound/about-yuv-video
HRESULT configure(com_ptr<IMFStreamDescriptor> stream) noexcept;

std::string to_string(const GUID& guid) noexcept;
std::string to_readable(const GUID& guid) noexcept;

winrt::hstring to_hstring(const GUID& guid) noexcept;

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
