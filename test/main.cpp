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
    fs::path asset_dir{ASSET_DIR};
    if (fs::exists(asset_dir))
        return asset_dir;
#endif
    return fs::current_path();
}

bool has_env(gsl::czstring<> key) noexcept {
    size_t len = 0;
    char buf[40]{};
    if (auto ec = getenv_s(&len, buf, key)) {
        spdlog::warn("getenv_s: {}", ec);
        spdlog::debug("key: {}", key);
        return false;
    }
    std::string_view value{buf, len};
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

    if (has_env("APPVEYOR"))
        spdlog::warn("for CI environment, some tests will be marked 'failed as expected'");

    spdlog::info("cpp_winrt: {}", CPPWINRT_VERSION);
    spdlog::info("media_foundation:");
    spdlog::info("  version: {:x}", MF_VERSION);
    return Catch::Session{}.run(argc, argv);
}

TEST_CASE("HRESULT format", "[format]") {
    const auto txt = to_readable(E_FAIL);
    CAPTURE(txt);
    REQUIRE(txt == "0x80004005");
}

TEST_CASE("GUID", "[format]") {
    SECTION("internal") {
        const GUID uuid0 = get_guid0();
        REQUIRE(to_string(uuid0) == "11790296-A926-45AB-96CB-A9CB187F37AD");
    }
    SECTION("Media Foundation SDK") {
        // search these values in Registry Editor
        REQUIRE(to_string(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID) == "8AC3587A-4AE7-42D8-99E0-0A6013EEF90F");
    }
}
