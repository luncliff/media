/**
 * @file    winrt_test.cpp
 * @author  github.com/luncliff (luncliff@gmail.com)
 */
#include <media.hpp>
#define CATCH_CONFIG_WINDOWS_CRTDBG
#include <catch2/catch.hpp>
#include <spdlog/spdlog.h>

// https://docs.microsoft.com/en-us/windows/win32/sysinfo/getting-the-system-version
#include <DispatcherQueue.h>
#include <VersionHelpers.h>

using namespace std::chrono_literals;
//using winrt::resume_foreground;
//using winrt::Windows::System::DispatcherQueueController;
//using winrt::Windows::System::DispatcherQueue;
//using winrt::Windows::Foundation::IAsyncOperation;

// https://devblogs.microsoft.com/oldnewthing/20191223-00/?p=103255
TEST_CASE("DispatcherQueueController(DQTYPE_THREAD_CURRENT/DQTAT_COM_ASTA)", "[WinRT][ABI]") {
    using ABI::Windows::Foundation::IAsyncAction;
    using ABI::Windows::System::IDispatcherQueueController;

    DispatcherQueueOptions options{};
    options.dwSize = sizeof(DispatcherQueueOptions);
    options.threadType = DISPATCHERQUEUE_THREAD_TYPE::DQTYPE_THREAD_CURRENT;
    options.apartmentType = DISPATCHERQUEUE_THREAD_APARTMENTTYPE::DQTAT_COM_ASTA;

    com_ptr<IDispatcherQueueController> controller{};
    REQUIRE(CreateDispatcherQueueController(options, controller.put()) == S_OK);
    auto queue = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
    REQUIRE(queue);

    // @todo implement a TC for this kind of delegate usage
    //SECTION("add delegate to queue") {
    //    queue.ShutdownCompleted(winrt::auto_revoke, [](auto sender, auto e) {
    //        UNREFERENCED_PARAMETER(sender, e);
    //        spdlog::warn("DispatcherQueue.ShutdownCompleted");
    //    });
    //}
    SECTION("create again") {
        com_ptr<IDispatcherQueueController> c2{};
        REQUIRE(CreateDispatcherQueueController(options, c2.put()) == RPC_E_WRONG_THREAD);
    }

    com_ptr<IAsyncAction> action{};
    REQUIRE(controller->ShutdownQueueAsync(action.put()) == S_OK);
    REQUIRE_NOTHROW(action.get());

    // https://docs.microsoft.com/en-us/uwp/api/windows.system.dispatcherqueuecontroller.shutdownqueueasync?view=winrt-18362
    // notice that the queue can be queried after shutdown...
    REQUIRE(winrt::Windows::System::DispatcherQueue::GetForCurrentThread());
    //REQUIRE(queue.TryEnqueue([]() {})); // enqueue still works. but it will be leaked
}

void shutdown(winrt::Windows::System::DispatcherQueueController controller) {
    auto operation = controller.ShutdownQueueAsync();
    REQUIRE_NOTHROW(operation.get());
}

TEST_CASE("DispatcherQueueController::CreateOnDedicatedThread", "[WinRT]") {
    auto current_queue = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
    REQUIRE(current_queue);
    auto controller = winrt::Windows::System::DispatcherQueueController::CreateOnDedicatedThread();
    auto worker_queue = controller.DispatcherQueue();
    REQUIRE(worker_queue);
    shutdown(controller);
    REQUIRE(worker_queue != current_queue);
}

// @see https://devblogs.microsoft.com/oldnewthing/20191210-00/?p=103197
// @see https://devblogs.microsoft.com/oldnewthing/20191223-00/?p=103255
[[nodiscard]] auto resume_on_queue(winrt::Windows::System::DispatcherQueue dispatcher) {
    struct awaitable final {
        winrt::Windows::System::DispatcherQueue m_dispatcher;
        bool m_queued = false;

        bool await_ready() const noexcept {
            return false;
        }
        bool await_suspend(coroutine_handle<> handle) noexcept {
            m_queued = m_dispatcher.TryEnqueue(handle);
            return m_queued;
        }
        bool await_resume() const noexcept {
            return m_queued;
        }
    };
    return awaitable{dispatcher};
}

/// @throws winrt::hresult_error
[[nodiscard]] auto query_thread_id(winrt::Windows::System::DispatcherQueue queue) noexcept(false)
    -> winrt::Windows::Foundation::IAsyncOperation<uint32_t> {
    co_await winrt::resume_foreground(queue);
    co_await resume_on_queue(queue);
    co_return GetCurrentThreadId();
}

/// @see https://gist.github.com/kennykerr/6490e1494449927147dc18616a5e601e
/// @todo use CreateOnDedicatedThread
TEST_CASE("DispatcherQueue(IAsyncOperation)", "[WinRT]") {
    auto controller = winrt::Windows::System::DispatcherQueueController::CreateOnDedicatedThread();
    auto on_return = gsl::finally([controller]() { shutdown(controller); });

    auto queue = controller.DispatcherQueue();
    REQUIRE(queue);
    auto current = GetCurrentThreadId();
    auto dedicated = query_thread_id(queue).get();
    REQUIRE(current != dedicated);
}

TEST_CASE("TimeZone", "[WinRT]") {
    TIME_ZONE_INFORMATION timezone{};
    auto id = GetTimeZoneInformation(&timezone);
    if (id == TIME_ZONE_ID_INVALID)
        winrt::throw_last_error();
    spdlog::debug("timezone:");
    spdlog::debug(" id: {}", id);
    spdlog::debug(" name: {}", winrt::to_string(timezone.StandardName));
    if (id == TIME_ZONE_ID_DAYLIGHT)
        spdlog::debug(" daylight: {}", winrt::to_string(timezone.DaylightName));
}

TEST_CASE("GetProcessInformation", "[Win32][!mayfail]") {
    HANDLE proc = GetCurrentProcess();
    SECTION("PROCESS_POWER_THROTTLING_STATE") {
        PROCESS_POWER_THROTTLING_STATE state{};
        if (GetProcessInformation(proc, ProcessPowerThrottling, &state, sizeof(state)))
            FAIL(GetLastError());
    }
    SECTION("APP_MEMORY_INFORMATION") {
        APP_MEMORY_INFORMATION info{};
        if (GetProcessInformation(proc, ProcessAppMemoryInfo, &info, sizeof(info)))
            FAIL(GetLastError());
    }
    SECTION("PROCESS_PROTECTION_LEVEL_INFORMATION") {
        PROCESS_PROTECTION_LEVEL_INFORMATION info{};
        if (GetProcessInformation(proc, ProcessProtectionLevelInfo, &info, sizeof(info)))
            FAIL(GetLastError());
    }
}

/// @see https://docs.microsoft.com/en-us/windows/win32/sysinfo/structure-of-the-registry
TEST_CASE("GetModuleFileName", "[Win32]") {
    HMODULE mod = GetModuleHandleW(NULL); // name of the .exe
    REQUIRE(mod);
    WCHAR buf[MAX_PATH]{};
    DWORD buflen = GetModuleFileNameW(mod, buf, MAX_PATH);
    REQUIRE(buflen);
    spdlog::debug("module: {}", winrt::to_string({buf, buflen})); // full path of the executable

    fs::path fpath{buf};
    REQUIRE(fs::is_regular_file(fpath));
}

void report_registry_operation_status(LSTATUS status) {
    switch (status) {
    case ERROR_SUCCESS: // 0
        break;
    case ERROR_ACCESS_DENIED: // 5
        return spdlog::error("{}", "ERROR_ACCESS_DENIED");
    case ERROR_INVALID_PARAMETER: // 87
        return spdlog::error("{}", "ERROR_INVALID_PARAMETER");
    default:
        return spdlog::error("{}", status);
    }
}

void set_value(HKEY hkey, winrt::hstring text) {
    if (auto ec = RegSetValueExW(hkey, NULL, 0, REG_SZ, //
                                 reinterpret_cast<const BYTE*>(text.data()),
                                 sizeof(winrt::hstring::value_type) * text.size()))
        spdlog::error("set(REG_SZ): {} {}", ec, winrt::to_string(text));
}

/// @brief set HKEY. REG_SZ with GUID string(include {})
void set_value(HKEY hkey, const GUID& guid) {
    constexpr auto bufsz = 40;
    wchar_t buf[bufsz]{};
    const auto buflen = static_cast<winrt::hstring::size_type>(StringFromGUID2(guid, buf, bufsz));
    return set_value(hkey, {buf, buflen});
}

void set_value(HKEY hkey, DWORD value) {
    if (auto ec = RegSetValueExW(hkey, NULL, 0, REG_DWORD, //
                                 reinterpret_cast<const BYTE*>(&value), sizeof(value)))
        spdlog::error("set(REG_DWORD): {} {}", ec, value);
}

/// @see https://docs.microsoft.com/en-us/windows/win32/sysinfo/structure-of-the-registry
TEST_CASE("Registry(HKEY_CLASSES_ROOT)", "[Win32][!mayfail]") {
    winrt::hstring subkey = L"keep.parctice.0";

    HKEY hkey{};
    if (auto ec = RegCreateKeyExW(HKEY_CLASSES_ROOT, subkey.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS,
                                  NULL, &hkey, NULL)) {
        report_registry_operation_status(ec);
        FAIL(ec);
    }
    REQUIRE(RegDeleteKeyExW(hkey, subkey.c_str(), KEY_WOW64_32KEY, 0) == ERROR_SUCCESS);
}

TEST_CASE("Registry(HKEY_LOCAL_MACHINE)", "[Win32][!mayfail]") {
    winrt::hstring subkey = L"keep.parctice.1";
    HKEY hkey{};
    if (auto ec = RegCreateKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS,
                                  NULL, &hkey, NULL)) {
        report_registry_operation_status(ec);
        FAIL(ec);
    }
    REQUIRE(RegDeleteKeyExW(hkey, subkey.c_str(), KEY_WOW64_32KEY, 0) == ERROR_SUCCESS);
}

/// @see https://github.com/microsoft/cppwinrt/blob/master/cppwinrt/cmd_reader.h
TEST_CASE("Registry(HKEY_CURRENT_USER) - close/delete", "[Win32]") {
    winrt::hstring subkey = L"keep.parctice.2";
    HKEY hkey{};
    if (auto ec = RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS,
                                  NULL, &hkey, NULL)) {
        report_registry_operation_status(ec);
        FAIL(ec);
    }
    SECTION("DWORD") {
        set_value(hkey, 0xBEAF);
    }
    REQUIRE(RegCloseKey(hkey) == ERROR_SUCCESS);
    REQUIRE(RegDeleteKeyExW(hkey, subkey.c_str(), KEY_WOW64_32KEY, 0) == ERROR_INVALID_HANDLE); // already closed
}

TEST_CASE("Registry(HKEY_CURRENT_USER) - delete/close", "[Win32]") {
    winrt::hstring subkey = L"keep.parctice.3";
    HKEY hkey{};
    if (auto ec = RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS,
                                  NULL, &hkey, NULL)) {
        report_registry_operation_status(ec);
        FAIL(ec);
    }
    SECTION("winrt::hstring") {
        const auto txt = to_hstring(get_IID_0());
        set_value(hkey, txt);
        REQUIRE(RegDeleteKeyExW(hkey, subkey.c_str(), KEY_WOW64_32KEY, 0) == ERROR_FILE_NOT_FOUND);
    }
    REQUIRE(RegCloseKey(hkey) == ERROR_SUCCESS);
}
