/**
 * @file    winrt_test.cpp
 * @author  github.com/luncliff (luncliff@gmail.com)
 */
#include <media.hpp>
#define CATCH_CONFIG_WINDOWS_CRTDBG
#include <catch2/catch.hpp>
#include <spdlog/spdlog.h>

#if __has_include(<DispatcherQueue.h>)
#include <DispatcherQueue.h>

using namespace std;
using namespace winrt;
using namespace winrt::Windows::System;
using namespace winrt::Windows::Foundation;

// https://devblogs.microsoft.com/oldnewthing/20191223-00/?p=103255
auto resume_foreground(DispatcherQueue const& dispatcher) {
    struct awaitable {
        DispatcherQueue m_dispatcher;
        bool m_queued = false;

        bool await_ready() const noexcept {
            return false;
        }
        bool await_suspend(coroutine_handle<> handle) noexcept {
            m_queued = m_dispatcher.TryEnqueue([handle] { handle(); });
            return m_queued;
        }
        bool await_resume() noexcept {
            return m_queued;
        }
    };
    return awaitable{dispatcher};
}

TEST_CASE("DispatcherQueueController(DQTYPE_THREAD_DEDICATED)") {
    using ABI::Windows::Foundation::IAsyncAction;
    using ABI::Windows::System::IDispatcherQueueController;

    com_ptr<IDispatcherQueueController> controller{};
    DispatcherQueueOptions options{};
    options.dwSize = sizeof(DispatcherQueueOptions);
    options.threadType = DISPATCHERQUEUE_THREAD_TYPE::DQTYPE_THREAD_DEDICATED; // create a 'CoreMessaging' thread
    options.apartmentType = DISPATCHERQUEUE_THREAD_APARTMENTTYPE::DQTAT_COM_ASTA;
    REQUIRE(CreateDispatcherQueueController(options, controller.put()) == S_OK);

    com_ptr<IAsyncAction> action{};
    REQUIRE(controller->ShutdownQueueAsync(action.put()) == S_OK);
    REQUIRE_NOTHROW(action.get());
}

TEST_CASE("DispatcherQueueController(DQTYPE_THREAD_CURRENT)") {
    using ABI::Windows::Foundation::IAsyncAction;
    using ABI::Windows::System::IDispatcherQueueController;

    com_ptr<IDispatcherQueueController> controller{};
    DispatcherQueueOptions options{};
    options.dwSize = sizeof(DispatcherQueueOptions);
    options.threadType = DISPATCHERQUEUE_THREAD_TYPE::DQTYPE_THREAD_CURRENT;
    options.apartmentType = DISPATCHERQUEUE_THREAD_APARTMENTTYPE::DQTAT_COM_ASTA;
    REQUIRE(CreateDispatcherQueueController(options, controller.put()) == S_OK);

    com_ptr<IAsyncAction> action{};
    REQUIRE(controller->ShutdownQueueAsync(action.put()) == S_OK);
    REQUIRE_NOTHROW(action.get());

    com_ptr<IDispatcherQueueController> controller2{};
    REQUIRE(CreateDispatcherQueueController(options, controller2.put()) == RPC_E_WRONG_THREAD);
}

/// @see https://gist.github.com/kennykerr/6490e1494449927147dc18616a5e601e
auto create_controller(DISPATCHERQUEUE_THREAD_TYPE thread_type) noexcept(false)
    -> winrt::Windows::System::DispatcherQueueController {
    DispatcherQueueOptions options{};
    options.dwSize = sizeof(DispatcherQueueOptions);
    options.threadType = thread_type;
    options.apartmentType = DISPATCHERQUEUE_THREAD_APARTMENTTYPE::DQTAT_COM_ASTA;
    DispatcherQueueController controller{nullptr};
    winrt::check_hresult(CreateDispatcherQueueController(
        options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(put_abi(controller))));
    return controller;
}

auto do_something(DispatcherQueue queue) -> IAsyncOperation<uint32_t> {
    try {
        co_await resume_foreground(queue);
        uint32_t thread_id = GetCurrentThreadId();
        co_return thread_id;
    } catch (const winrt::hresult_error& ex) {
        co_return ex.code();
    } catch (...) {
        co_return UINT32_MAX;
    }
}

TEST_CASE("DispatcherQueue(DQTYPE_THREAD_DEDICATED)") {
    // create a 'CoreMessaging' thread
    auto controller = create_controller(DISPATCHERQUEUE_THREAD_TYPE::DQTYPE_THREAD_DEDICATED);
    auto queue = controller.DispatcherQueue();
    REQUIRE(queue);

    auto current = GetCurrentThreadId();
    auto worker = do_something(queue).get();
    REQUIRE(worker != current);

    REQUIRE_NOTHROW(controller.ShutdownQueueAsync().get());
}

TEST_CASE("DispatcherQueue(DQTYPE_THREAD_CURRENT)", "[!mayfail]") {
    try {
        auto controller = create_controller(DISPATCHERQUEUE_THREAD_TYPE::DQTYPE_THREAD_CURRENT);
        auto queue = controller.DispatcherQueue();
        REQUIRE(queue);

        auto current = GetCurrentThreadId();
        auto worker = do_something(queue).get();
        REQUIRE(worker == current);

        REQUIRE_NOTHROW(controller.ShutdownQueueAsync().get());
    } catch (const winrt::hresult_error& ex) {
        CAPTURE(ex.code());
        FAIL(winrt::to_string(ex.message()));
    }
}

#endif
