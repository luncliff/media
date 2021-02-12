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
