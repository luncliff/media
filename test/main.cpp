#define CATCH_CONFIG_RUNNER
#define CATCH_CONFIG_WINDOWS_CRTDBG
#include <catch2/catch.hpp>

#include <filesystem>
#include <media.hpp>

using namespace std;
namespace fs = std::filesystem;

fs::path get_asset_dir() noexcept {
#if defined(ASSET_DIR)
    return {ASSET_DIR};
#else
    return fs::current_path();
#endif
}

/// @todo catch `winrt::hresult_error`
int main(int argc, char* argv[]) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    auto on_exit = gsl::finally(&winrt::uninit_apartment);
    Catch::Session suite{};
    return suite.run(argc, argv);
}
