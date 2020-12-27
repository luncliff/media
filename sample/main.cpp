#include <media.hpp>
#include <spdlog/spdlog.h>

#include <DispatcherQueue.h>

auto create_controller(DISPATCHERQUEUE_THREAD_TYPE thread_type) noexcept(false)
    -> winrt::Windows::System::DispatcherQueueController {
    DispatcherQueueOptions options{};
    options.dwSize = sizeof(DispatcherQueueOptions);
    options.threadType = thread_type;
    options.apartmentType = DISPATCHERQUEUE_THREAD_APARTMENTTYPE::DQTAT_COM_ASTA;
    ABI::Windows::System::IDispatcherQueueController* ptr{};
    winrt::check_hresult(CreateDispatcherQueueController(options, &ptr));
    return {ptr, winrt::take_ownership_from_abi};
}

winrt::Windows::System::DispatcherQueueController main_controller{nullptr};
winrt::Windows::System::DispatcherQueue main_queue{nullptr};

auto get_main_queue() -> winrt::Windows::System::DispatcherQueue {
    return main_queue;
}

int init(int argc, wchar_t* argv[], wchar_t* envp[]);

// todo: use WinMain?
int wmain(int argc, wchar_t* argv[], wchar_t* envp[]) {
    winrt::init_apartment();
    auto on_exit = gsl::finally(&winrt::uninit_apartment);
    try {
        if (auto ec = init(argc, argv, envp))
            return ec;

        main_controller = create_controller(DISPATCHERQUEUE_THREAD_TYPE::DQTYPE_THREAD_CURRENT);
        main_queue = main_controller.DispatcherQueue();
        //auto shutdown = controller.ShutdownQueueAsync();
        //shutdown.get();

    } catch (const winrt::hresult_error& ex) {
        print_error(ex);
        return ex.code();
    }
    return EXIT_SUCCESS;
}
