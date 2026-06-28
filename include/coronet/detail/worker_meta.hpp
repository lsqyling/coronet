#pragma once

#include "coronet/config/io_context.hpp"
#include "coronet/detail/spsc_cursor.hpp"
#include "coronet/detail/task_info.hpp"
#include "coronet/platform/platform.hpp"

// Compile-time platform proactor selection (no virtual dispatch)
#if defined(CORONET_PLATFORM_WINDOWS)
#include "coronet/platform/iocp/iocp_proactor.hpp"
namespace coronet::detail { using proactor_type = platform::iocp::iocp_proactor; }
#else
#include "coronet/platform/io_uring/io_uring_proactor.hpp"
namespace coronet::detail { using proactor_type = platform::io_uring::io_uring_proactor; }
#endif

#include <coroutine>
#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

namespace coronet::detail {

/// Per-worker (per-io_context) state.
/// Manages the platform proactor + SPSC reap_swap ring + cross-thread spawn queue.
struct worker_meta {
    // ---- the platform proactor (concrete type, no virtual dispatch) ----
    proactor_type* proactor{nullptr};

    // ---- reap_swap (SPSC ring for same-thread coroutine handles) ----
    // Heap-allocated via vector to avoid stack overflow with multiple
    // io_context instances (each is config::swap_capacity * 8 bytes ≈ 131KB).
    std::vector<std::coroutine_handle<>> reap_swap{config::swap_capacity};

    spsc_cursor<config::cur_t, config::swap_capacity> reap_cur;

    // ---- cross-thread spawn queue (mutex-protected) ----
    alignas(config::cache_line_size)
    std::mutex cross_mtx;
    std::vector<std::coroutine_handle<>> cross_queue;

    // ---- submission tracking ----
    int32_t  requests_to_reap   = 0;
    uint32_t requests_to_submit = 0;

    // ---- identity ----
    config::ctx_id_t ctx_id{0};

    // ---- lifecycle ----
    void init(uint32_t entries);
    void deinit() noexcept;

    // ---- coroutine spawn ----
    void co_spawn_unsafe(std::coroutine_handle<> handle) noexcept;
    void co_spawn_auto(std::coroutine_handle<> handle) noexcept;

    /// Thread-safe cross-thread spawn: pushes to cross_queue + wakes up target.
    void co_spawn_cross(std::coroutine_handle<> handle) noexcept;

    // ---- scheduling ----
    std::coroutine_handle<> schedule() noexcept;
    void forward_task(std::coroutine_handle<> handle) noexcept;
    void work_once();

    /// Drain cross-thread queue into the SPSC ring. Called from the event loop.
    void drain_cross_thread() noexcept;

    // ---- I/O submission & completion ----
    void poll_submission() noexcept;
    uint32_t poll_completion() noexcept;
    void handle_completion(const platform::completion_info* info) noexcept;

    // ---- helpers ----
    void check_submission_threshold() noexcept;

    [[nodiscard]]
    bool has_task_ready() const noexcept {
        return !reap_cur.empty();
    }
};

} // namespace coronet::detail
