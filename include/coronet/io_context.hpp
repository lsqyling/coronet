#pragma once

#include "coronet/config/io_context.hpp"
#include "coronet/detail/worker_meta.hpp"
#include "coronet/detail/thread_meta.hpp"
#include "coronet/detail/io_context_meta.hpp"
#include "coronet/task.hpp"

// Platform-specific proactor: compile-time selection (no virtual dispatch)
#if defined(CORONET_PLATFORM_WINDOWS)
#include "coronet/platform/iocp/iocp_proactor.hpp"
#else
#include "coronet/platform/io_uring/io_uring_proactor.hpp"
#endif

#include <thread>
#include <atomic>

namespace coronet {

/// The core event loop / scheduler — single-threaded, one per io_context.
///
/// Uses compile-time platform selection:
///   Linux  → io_uring_proactor
///   Windows → iocp_proactor
///
/// No virtual dispatch, no heap allocation for the proactor.
///
/// Usage:
///   io_context ctx;
///   ctx.co_spawn(my_task());
///   ctx.start();
///   ctx.join();
///
class [[nodiscard]] io_context final {
public:
#if defined(CORONET_PLATFORM_WINDOWS)
    using proactor_type = platform::iocp::iocp_proactor;
#else
    using proactor_type = platform::io_uring::io_uring_proactor;
#endif

    io_context() noexcept;
    ~io_context() noexcept;

    // Non-copyable, non-movable
    io_context(const io_context&) = delete;
    io_context(io_context&&) = delete;
    io_context& operator=(const io_context&) = delete;
    io_context& operator=(io_context&&) = delete;

    // ---- lifecycle ----
    void start();
    void join();

    /// Request graceful stop (thread-safe).
    void can_stop() noexcept {
        will_stop_ = true;
        proactor_.wakeup();
    }

    /// Spawn a task onto this io_context (thread-safe).
    void co_spawn(task<void>&& entrance) noexcept;

    // ---- accessors ----
    config::ctx_id_t id() const noexcept { return id_; }
    proactor_type& proactor() noexcept { return proactor_; }
    const proactor_type& proactor() const noexcept { return proactor_; }

    /// Spawn a raw coroutine handle (used by synchronization primitives).
    void spawn_handle(std::coroutine_handle<> handle) noexcept {
        worker_.co_spawn_auto(handle);
    }

private:
    void deinit() noexcept;
    void run();

    void do_worker_part();
    void do_submission_part() noexcept;
    void do_completion_part() noexcept;

    // ---- data ----
    proactor_type proactor_;            // concrete type, stack-allocated, zero virtual dispatch
    detail::worker_meta worker_;
    std::thread host_thread_;
    config::ctx_id_t id_;
    std::atomic<bool> will_stop_{false};
};

/// Free function: spawn a task on the current thread's io_context.
void co_spawn(task<void>&& entrance) noexcept;

/// Get the current thread's io_context.
io_context& this_io_context() noexcept;

} // namespace coronet
