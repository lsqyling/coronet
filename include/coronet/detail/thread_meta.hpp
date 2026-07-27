#pragma once

#include "coronet/config/io_context.hpp"
#include <coroutine>

namespace coronet { class io_context; }
namespace coronet::detail { struct worker_meta; }

namespace coronet::detail {

/// Per-thread metadata: gives O(1) access to the current io_context and worker.
struct alignas(config::cache_line_size) thread_meta {
    /// The io_context this thread is running
    io_context* ctx = nullptr;

    /// The worker_meta (cache-line-aligned member of io_context)
    worker_meta* worker = nullptr;

    /// io_context id (unique among all io_contexts)
    config::ctx_id_t ctx_id = config::ctx_id_t(-1);
};

/// Thread-local current context
extern thread_local thread_meta this_thread;

/// 将协程句柄调度到当前线程的 worker 上恢复。
/// 供不希望依赖 worker_meta 完整定义的调用点（如 trivial_task）使用，
/// 实现位于 worker_meta.cpp。
/// Schedule a coroutine handle onto the current thread's worker for resumption.
/// Used by call sites that wish to avoid depending on worker_meta's full definition.
void schedule_on_this_thread(std::coroutine_handle<> handle) noexcept;

} // namespace coronet::detail
