#include "coronet/io_context.hpp"
#include "coronet/detail/worker_meta.hpp"
#include "coronet/detail/thread_meta.hpp"
#include "coronet/detail/io_context_meta.hpp"
#include "coronet/log/log.hpp"

#if defined(CORONET_PLATFORM_WINDOWS)
#include <winsock2.h>
#include <mutex>
#endif

#include <cstdio>
#include <cstdlib>

namespace coronet {

// ---- Windows Winsock init ----
#if defined(CORONET_PLATFORM_WINDOWS)
namespace {
    std::once_flag g_wsa_init_flag;
    void init_winsock() {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            std::fprintf(stderr, "FATAL: WSAStartup failed\n");
            std::abort();
        }
    }
}
#endif

// ---- io_context ----

io_context::io_context() noexcept {
#if defined(CORONET_PLATFORM_WINDOWS)
    std::call_once(g_wsa_init_flag, init_winsock);
#endif
    id_ = detail::g_io_context_meta.create_count.fetch_add(1,
        std::memory_order_relaxed);
    log::d("[io_context] constructor: id=%u\n", id_);

    // Proactor is a concrete stack-allocated member (no heap alloc, no virtual dispatch)
    worker_.proactor = &proactor_;
    proactor_.init(config::default_io_uring_entries);
    worker_.ctx_id = id_;
    log::d("[io_context] constructor done\n");
}

io_context::~io_context() noexcept {
    can_stop();
    join();
}

void io_context::deinit() noexcept {
    proactor_.deinit();
}

void io_context::start() {
    log::d("[io_context] start() — spawning thread\n");
    detail::g_io_context_meta.ready_count.fetch_add(1,
        std::memory_order_release);
    host_thread_ = std::thread(&io_context::run, this);
}

void io_context::join() {
    if (host_thread_.joinable()) {
        host_thread_.join();
    }
}

void io_context::co_spawn(task<void>&& entrance) noexcept {
    auto handle = entrance.get_handle();
    log::d("[io_context] co_spawn: handle=%p\n", handle.address());
    entrance.detach();
    worker_.co_spawn_auto(handle);
}

void io_context::run() {
    log::i("[io_context] run() — thread started, id=%u\n", id_);

    // Set thread-local
    detail::this_thread.ctx = this;
    detail::this_thread.worker = &worker_;
    detail::this_thread.ctx_id = id_;

    // Wait for all io_contexts to be ready (barrier)
    detail::g_io_context_meta.wait_all_ready();
    log::d("[io_context] run() — barrier passed, entering main loop\n");

    uint64_t loop_count = 0;
    while (!will_stop_.load(std::memory_order_relaxed)) [[likely]] {
        loop_count++;
        if (loop_count <= 5 || loop_count % 1000 == 0) {
            log::v("[io_context] loop #%llu\n", (unsigned long long)loop_count);
        }
        worker_.drain_cross_thread();  // 0. drain cross-thread spawn queue
        do_worker_part();               // 1. resume ready coroutines
        do_submission_part();           // 2. submit batched I/O (io_uring only)
        do_completion_part();           // 3. reap completions
    }

    log::i("[io_context] run() — loop exited after %llu iterations\n",
           (unsigned long long)loop_count);
    deinit();
    detail::this_thread = {};
}

void io_context::do_worker_part() {
    while (auto handle = worker_.schedule()) {
        handle.resume();
    }
}

void io_context::do_submission_part() noexcept {
#if defined(CORONET_USE_IOURING)
    // io_uring: submit batched SQEs
    worker_.poll_submission();
#else
    // epoll / IOCP: no-op — operations are issued in await_suspend
    (void)0;
#endif
}

void io_context::do_completion_part() noexcept {
    worker_.poll_completion();
}

// ---- free functions ----

void co_spawn(task<void>&& entrance) noexcept {
    auto ctx = detail::this_thread.ctx;
    if (ctx) {
        ctx->co_spawn(std::move(entrance));
    }
}

io_context& this_io_context() noexcept {
    return *detail::this_thread.ctx;
}

} // namespace coronet
