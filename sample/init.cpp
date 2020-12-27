#include <media.hpp>
#include <spdlog/spdlog.h>

#include <clocale>
#include <strsafe.h>

HRESULT split(gsl::cwzstring<> txt, winrt::hstring& key, winrt::hstring& value) {
    auto bufsz = lstrlenW(txt) + 2;
    auto buf = std::make_unique<wchar_t[]>(bufsz);
    if (auto hr = StringCchCopyW(buf.get(), bufsz, txt); FAILED(hr))
        return hr;
    constexpr auto delims = L"=";
    wchar_t* context{};
    key = wcstok_s(buf.get(), delims, &context);
    value = wcstok_s(context, delims, &context);
    return S_OK;
}

int init(int argc, wchar_t* argv[], wchar_t* envp[]) {
    std::setlocale(LC_ALL, ".65001");

    spdlog::set_level(spdlog::level::level_enum::trace);
    spdlog::info("argv:");
    for (auto i = 0; i < argc; ++i) {
        spdlog::info(" - \"{}\"", winrt::to_string(argv[i]));
    }

    spdlog::info("env:");
    for (auto i = 0; envp[i]; ++i) {
        winrt::hstring key{}, value{};
        if SUCCEEDED (split(envp[i], key, value))
            spdlog::info(" - {}: \"{}\"", winrt::to_string(key), winrt::to_string(value));
    }
    return EXIT_SUCCESS;
}
