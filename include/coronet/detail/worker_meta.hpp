#pragma once
// ============================================================
// worker_meta.hpp — 每个 io_context 的调度器元数据
// ============================================================
// 管理平台 Proactor + SPSC reap_swap 环 + 跨线程生成队列。
// 核心数据结构：
//   - reap_swap（SPSC 环）：同线程协程句柄调度，lock-free
//   - cross_queue（互斥锁保护）：跨线程 co_spawn 队列
//   - requests_to_reap / requests_to_submit：I/O 操作追踪计数器
//
// Per-worker (per-io_context) state.
// Manages the platform proactor + SPSC reap_swap ring + cross-thread spawn queue.

#include "coronet/config/io_context.hpp"
#include "coronet/detail/spsc_cursor.hpp"
#include "coronet/detail/task_info.hpp"
#include "coronet/platform/platform.hpp"

// 编译期平台 Proactor 选择（零虚表分派）
// Compile-time platform proactor selection (no virtual dispatch)
#if defined(CORONET_PLATFORM_WINDOWS)
#include "coronet/platform/iocp/iocp_proactor.hpp"
namespace coronet::detail { using proactor_type = platform::iocp::iocp_proactor; }
#elif defined(CORONET_USE_IOURING)
#include "coronet/platform/io_uring/io_uring_proactor.hpp"
namespace coronet::detail { using proactor_type = platform::io_uring::io_uring_proactor; }
#else
#include "coronet/platform/epoll/epoll_reactor.hpp"
namespace coronet::detail { using proactor_type = platform::epoll::epoll_proactor; }
#endif

#include <coroutine>
#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

namespace coronet::detail {

/// 每个 worker（每个 io_context）的调度状态。
/// 包含 Proactor 指针、SPSC 环、跨线程队列和 I/O 计数器。
struct worker_meta {
    // ---- 平台 Proactor（具体类型指针，零虚表分派） ----
    // the platform proactor (concrete type, no virtual dispatch)
    proactor_type* proactor{nullptr};

    // ---- reap_swap：同线程协程句柄 SPSC 无锁环 ----
    // 用 vector 堆分配，避免多个 io_context 实例时栈溢出
    // （每个实例 = config::swap_capacity * 8 bytes ≈ 131KB）
    //
    // Heap-allocated via vector to avoid stack overflow with multiple
    // io_context instances (each is config::swap_capacity * 8 bytes ≈ 131KB).
    std::vector<std::coroutine_handle<>> reap_swap{config::swap_capacity};

    // SPSC 环游标（生产者/消费者指针）
    spsc_cursor<config::cur_t, config::swap_capacity> reap_cur;

    // ---- 跨线程生成队列（互斥锁保护） ----
    // 当其他线程调用 co_spawn_auto 时，句柄进入此队列，
    // 然后通过 eventfd / PostQueuedCompletionStatus 唤醒本 worker。
    //
    // cross-thread spawn queue (mutex-protected).
    // When another thread calls co_spawn_auto, handles land here
    // and the worker is woken via eventfd / PostQueuedCompletionStatus.
    alignas(config::cache_line_size)
    std::mutex cross_mtx;
    std::vector<std::coroutine_handle<>> cross_queue;

    // ---- I/O 提交追踪 ----
    // requests_to_reap：   待收割的 inflight 操作数（每次 await 构造 +1，完成时 -1）
    // requests_to_submit： 待提交的批处理操作数（每次 await 构造 +1，poll_submission 清零）
    int32_t  requests_to_reap   = 0;
    uint32_t requests_to_submit = 0;

    // ---- 身份标识 ----
    config::ctx_id_t ctx_id{0};

    // ---- 生命周期 / lifecycle ----
    void init(uint32_t entries);
    void deinit() noexcept;

    // ---- 协程生成 / coroutine spawn ----

    /// 不检查线程，直接推入 SPSC 环（调用者保证同线程）
    void co_spawn_unsafe(std::coroutine_handle<> handle) noexcept;

    /// 自动判断线程：同线程 → SPSC 环，跨线程 → cross_queue + wakeup
    void co_spawn_auto(std::coroutine_handle<> handle) noexcept;

    /// 线程安全的跨线程生成：推入 cross_queue + 唤醒目标 worker
    /// Thread-safe cross-thread spawn: pushes to cross_queue + wakes up target.
    void co_spawn_cross(std::coroutine_handle<> handle) noexcept;

    // ---- 调度 / scheduling ----

    /// 从 SPSC 环弹出一个就绪协程句柄（消费者）
    std::coroutine_handle<> schedule() noexcept;

    /// 将协程句柄推入 SPSC 环（生产者）
    void forward_task(std::coroutine_handle<> handle) noexcept;

    /// 取出一个就绪协程并恢复执行
    void work_once();

    /// 将跨线程队列中的句柄搬移到 SPSC 环。事件循环每轮调用。
    /// Drain cross-thread queue into the SPSC ring. Called from the event loop.
    void drain_cross_thread() noexcept;

    // ---- I/O 提交与收割 / I/O submission & completion ----

    /// 提交批量 I/O（仅 io_uring；epoll/IOCP 为 no-op）
    void poll_submission() noexcept;

    /// 收割一个 I/O 完成事件（调用 Proactor::wait_completion）
    uint32_t poll_completion() noexcept;

    /// 处理单个完成事件：解码 task_info → 设置结果 → 链式检查 → forward_task
    void handle_completion(const platform::completion_info* info) noexcept;

    // ---- 辅助 / helpers ----

    /// 检查是否达到提交阈值（当前阈值设为无限，实际不使用）
    void check_submission_threshold() noexcept;

    /// SPSC 环中是否有就绪协程
    [[nodiscard]]
    bool has_task_ready() const noexcept {
        return !reap_cur.empty();
    }
};

} // namespace coronet::detail
