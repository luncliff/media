/**
 * @file main.cpp
 * @author github.com/luncliff (luncliff@gmail.com)
 * 
 * @see https://docs.microsoft.com/en-us/windows/win32/medfound/processing-media-data-with-the-source-reader
 * @see https://docs.microsoft.com/en-us/windows/win32/medfound/using-the-source-reader-in-asynchronous-mode
 * @see https://docs.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsourcereadercallback-onreadsample
 * @see https://docs.microsoft.com/en-us/windows/win32/api/mfreadwrite/ne-mfreadwrite-mf_source_reader_flag
 */
#include <media.hpp>
#define CATCH_CONFIG_RUNNER
#define CATCH_CONFIG_WINDOWS_CRTDBG
#include <catch2/catch.hpp>
#include <catch2/catch_reporter_sonarqube.hpp>
#include <clocale>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

fs::path get_asset_dir() noexcept {
#if defined(ASSET_DIR)
    fs::path dirpath{ASSET_DIR};
    if (fs::exists(dirpath))
        return dirpath;
#endif
    return fs::current_path();
}

void get_env(gsl::cwzstring<> key, std::wstring& buf) noexcept {
    constexpr auto cap = MAX_PATH;
    buf.resize(cap);
    const DWORD len = GetEnvironmentVariableW(key, buf.data(), cap);
    if (len == 0)
        return buf.clear(); // ERROR_ENVVAR_NOT_FOUND
    else if (len > cap) {
        buf.resize(len);
        GetEnvironmentVariableW(key, buf.data(), len);
    }
}
bool has_env(gsl::cwzstring<> key) noexcept {
    std::wstring value{};
    get_env(key, value);
    return value.empty() == false;
}

/// @todo catch `winrt::hresult_error`
int main(int argc, char* argv[]) {
    std::setlocale(LC_ALL, ".65001");
    winrt::init_apartment();
    auto on_exit = gsl::finally(&winrt::uninit_apartment);

    //auto log = spdlog::basic_logger_st("report", "log.yaml");
    //spdlog::set_default_logger(log);
    spdlog::set_pattern("[%^%l%$] %v");
    spdlog::set_level(spdlog::level::level_enum::debug);

    if (has_env(L"APPVEYOR"))
        spdlog::warn("for CI environment, some tests will be marked 'failed as expected'");

    spdlog::info("cpp_winrt: {}", CPPWINRT_VERSION);
    spdlog::info("media_foundation: {:x}", MF_VERSION);
    return Catch::Session{}.run(argc, argv);
}

TEST_CASE("HRESULT format", "[format]") {
    const auto txt = to_readable(E_FAIL);
    CAPTURE(txt);
    REQUIRE(txt == "0x80004005");
}

TEST_CASE("GUID", "[format]") {
    SECTION("internal") {
        REQUIRE(to_string(get_IID_0()) == "11790296-A926-45AB-96CB-A9CB187F37AD");
        REQUIRE(to_string(get_CLSID_MFT()) == "1C2CE17A-FAAD-4E73-85E7-167068093F25");
    }
    SECTION("Media Foundation SDK") {
        // search these values in Registry Editor
        REQUIRE(to_string(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID) == "8AC3587A-4AE7-42D8-99E0-0A6013EEF90F");
    }
}

TEST_CASE("Custom MFT", "[COM]") {
    auto on_return = media_startup();
    HMODULE lib = LoadLibraryW(L"custom_mft.dll");
    REQUIRE(lib);
    auto on_exit = gsl::finally([lib]() { FreeLibrary(lib); });

    SECTION("DLL life functions are not exported") {
        REQUIRE(GetProcAddress(lib, "get_current_count"));
        REQUIRE_FALSE(GetProcAddress(lib, "DllAddRef"));
        REQUIRE_FALSE(GetProcAddress(lib, "DllRelease"));
        REQUIRE(GetProcAddress(lib, "DllGetClassObject"));
        REQUIRE(GetProcAddress(lib, "DllCanUnloadNow"));
        REQUIRE(GetProcAddress(lib, "DllRegisterServer"));
        REQUIRE(GetProcAddress(lib, "DllUnregisterServer"));
    }
    SECTION("CoCreateInstance(CLSCTX_INPROC)") {
        const GUID CLSID = get_CLSID_MFT();
        com_ptr<IUnknown> unknown{};
        HRESULT hr = CoCreateInstance(CLSID, NULL, CLSCTX_INPROC, IID_PPV_ARGS(unknown.put()));
        REQUIRE(hr == REGDB_E_CLASSNOTREG);
    }
}
