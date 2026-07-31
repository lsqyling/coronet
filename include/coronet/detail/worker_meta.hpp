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
// 平台 Proactor：通过桥接头集中选择（同时带入 platform.hpp）
// Platform proactor: selected via the proactor_selector bridge (also brings in platform.hpp)
#include "coronet/platform/proactor_selector.hpp"

#include <coroutine>
#include <cstdint>
#include <mutex>
#include <vector>

namespace coronet::detail {

/// 每个 worker（每个 io_context）的调度状态。
/// 包含 Proactor 指针、SPSC 环、跨线程队列和 I/O 计数器。
///
/// 缓存布局设计（hot/cold separation）：
/// - 热数据（proactor, 计数器, SPSC 环）放在结构体头部，事件循环每轮访问
/// - 冷数据（cross_mtx, cross_queue）用 alignas 隔离到独立缓存行
/// - 这样跨线程 co_spawn 获取 cross_mtx 时不会导致热数据缓存行失效
struct worker_meta {
    // ---- 热数据：事件循环每轮访问 ----
    // Hot data: accessed every event loop iteration.
    // Placed first for best cache locality.
    alignas(config::cache_line_size)
    platform::proactor_type* proactor{nullptr};

    // requests_to_reap:   inflight ops count (await +1, completion -1)
    // requests_to_submit: pending batch submissions (poll_submission resets)
    int32_t  requests_to_reap   = 0;
    uint32_t requests_to_submit = 0;

    config::ctx_id_t ctx_id{0};

    // ---- SPSC 环：同线程协程调度（热，仅事件循环线程访问）----
    // Same-thread coroutine scheduling ring (hot, single-thread only).
    // 用 vector 堆分配，避免多个 io_context 实例时栈溢出
    // （每个实例 = config::swap_capacity * 8 bytes ≈ 131KB）
    std::vector<std::coroutine_handle<>> reap_swap{config::swap_capacity};

    // SPSC 环游标（生产者/消费者指针）
    spsc_cursor<config::cur_t, config::swap_capacity> reap_cur;

    // ---- 冷数据：仅跨线程 co_spawn 访问 ----
    // Cold data: only accessed during cross-thread co_spawn.
    // alignas ensures cold data starts on a fresh cache line,
    // preventing false sharing with hot counters above.
    // 当其他线程调用 co_spawn_auto 时，句柄进入此队列，
    // 然后通过 eventfd / PostQueuedCompletionStatus 唤醒本 worker。
    alignas(config::cache_line_size)
    std::mutex cross_mtx;
    std::vector<std::coroutine_handle<>> cross_queue;

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

    /// 将协程句柄推入 SPSC 环（生产者操作）。
    /// P2-4: SPSC 环使用非原子操作，仅可从事件循环线程调用。
    /// Debug 断言会检测跨线程误用。
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

    // ---- 延迟任务队列（同线程，无锁）----
    // Deferred task queue for coroutines that should resume in the next
    // event loop iteration (e.g. yield). Same-thread access only, no lock needed.
    // P2-5: Prevents yield() from livelocking do_worker_part()'s while loop.
    // 延迟任务队列：需要在下一轮事件循环中恢复的协程（如 yield）。
    // 仅从事件循环线程访问，无需锁。
    // P2-5: 防止 yield() 在 do_worker_part() 的 while 循环中造成活锁。
    std::vector<std::coroutine_handle<>> deferred_tasks;

    /// 将协程句柄加入延迟队列（下一轮事件循环才恢复）
    /// Defer a coroutine handle to the next event loop iteration.
    void defer_task(std::coroutine_handle<> handle) noexcept {
        deferred_tasks.push_back(handle);
    }

    /// 将延迟队列中的句柄搬移到 SPSC 环。事件循环每轮调用。
    /// Drain deferred queue into the SPSC ring. Called from the event loop.
    void drain_deferred() noexcept {
        for (auto h : deferred_tasks) {
            forward_task(h);
        }
        deferred_tasks.clear();
    }
};

} // namespace coronet::detail
