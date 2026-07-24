// ============================================================
// worker_meta.cpp — 每个 io_context 的调度器元数据实现
// ============================================================
// 核心调度功能：
//   - SPSC 无锁环（reap_swap）：同线程协程句柄调度，无竞争
//   - 跨线程队列（cross_queue）：其他线程 co_spawn 的入口
//   - 完成事件处理（handle_completion）：解码 user_data → 恢复协程
//
// 线程安全分层设计：
//   1. 同线程 co_spawn → forward_task（SPSC push，无锁）
//   2. 跨线程 co_spawn → co_spawn_cross（mutex queue + wakeup）
//   3. 事件循环 drain_cross_thread → forward_task（批量搬移）

#include "coronet/detail/worker_meta.hpp"
#include "coronet/detail/io_context_meta.hpp"
#include "coronet/detail/thread_meta.hpp"
#include "coronet/log/log.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>

#if defined(CORONET_PLATFORM_WINDOWS)
#include "coronet/platform/iocp/iocp_win_io.hpp"  // win_chain_base for typed chain dispatch
#else
#include "coronet/platform/epoll/epoll_lazy_io.hpp"  // epoll_chain_base for typed chain dispatch
#endif

namespace coronet::detail {

// ---- 生命周期 ----

// init 和 deinit 委托给 Proactor，worker_meta 本身主要管理调度队列
void worker_meta::init(uint32_t entries) {
    assert(proactor);
    proactor->init(entries);
}

void worker_meta::deinit() noexcept {
    if (proactor) proactor->deinit();
}

// ---- 协程句柄投递 ----

// 不检查线程来源，直接推入 SPSC 环（调用者必须保证同线程调用）
// 这是最快路径，用于事件循环内部和已知同线程的场景
void worker_meta::co_spawn_unsafe(std::coroutine_handle<> handle) noexcept {
    forward_task(handle);
}

// 自动检测线程来源，选择最优路径：
//   同线程 → SPSC 环（无锁，O(1)）
//   跨线程 → mutex 保护队列 + eventfd/PQCS 唤醒（需要锁，但唤醒次数优化到最少）
void worker_meta::co_spawn_auto(std::coroutine_handle<> handle) noexcept {
    // If called from another thread, use the thread-safe cross-thread path
    // 判断条件：当前线程的 worker 不是自己，且全局屏障已通过
    // （ready_count > 0 意味着其他上下文的 run() 已启动）
    if (detail::this_thread.worker != this && detail::g_io_context_meta.ready_count > 0) {
        co_spawn_cross(handle);
        return;
    }
    // 同线程：直接 SPSC push
    forward_task(handle);
}

// 线程安全的跨线程投递：推入 mutex 保护队列 + 唤醒目标 worker
// 唤醒优化：只在队列从空变为非空时唤醒，避免高并发下重复唤醒
void worker_meta::co_spawn_cross(std::coroutine_handle<> handle) noexcept {
    bool need_wakeup = false;
    {
        std::lock_guard lock(cross_mtx);
        need_wakeup = cross_queue.empty();
        if (cross_queue.capacity() < 64) cross_queue.reserve(64);
        cross_queue.push_back(handle);
    }
    // Only wake the worker if queue was previously empty.
    // Eliminates redundant PostQueuedCompletionStatus calls at high concurrency.
    // 只有在队列从空变为非空时才唤醒目标线程。
    // 如果队列已有待处理项，说明目标线程已被标记唤醒，无需重复触发。
    // 这对高性能场景很重要 —— 减少 eventfd 写/PQCS 调用的系统开销。
    if (need_wakeup && proactor) {
        proactor->wakeup();
    }
}

// 将跨线程队列中的句柄批量搬移到 SPSC 环。
// 使用 thread_local 临时向量做无锁交换，最小化持有 mutex 的时间。
void worker_meta::drain_cross_thread() noexcept {
    if (cross_queue.empty()) return;

    // thread_local batch 避免每次分配内存
    thread_local std::vector<std::coroutine_handle<>> batch;
    {
        std::lock_guard lock(cross_mtx);
        // swap 而非 copy：O(1) 且不分配内存
        batch.swap(cross_queue);
    }
    // 批量推入 SPSC 环（无锁操作）
    for (auto h : batch) {
        forward_task(h);
    }
    batch.clear();
}

// ---- SPSC 调度 ----

// 从 SPSC 环弹出一个就绪协程句柄（消费者操作）
// 返回 nullptr 表示队列空
std::coroutine_handle<> worker_meta::schedule() noexcept {
    config::cur_t slot = reap_cur.pop();
    if (slot == config::cur_t(-1)) [[unlikely]]
        return nullptr;
    return reap_swap[slot];
}

// 将协程句柄推入 SPSC 环（生产者操作）
// SPSC 环满时是致命错误 —— 意味着有太多协程同时就绪，需要增大 swap_capacity
void worker_meta::forward_task(std::coroutine_handle<> handle) noexcept {
    config::cur_t slot = reap_cur.push();
    if (slot == config::cur_t(-1)) [[unlikely]] {
        // 环溢出意味着设计容量不足，直接终止以避免静默丢失协程句柄
        log::e("worker_meta: reap_swap overflow!\n");
        std::abort();
    }
    reap_swap[slot] = handle;
}

// 弹出一个就绪协程并恢复执行（便捷接口，主要用于测试）
void worker_meta::work_once() {
    auto handle = schedule();
    if (handle) handle.resume();
}

// ---- I/O 提交与收割 ----

// 提交批量 I/O 操作到内核。仅 io_uring 需要：将 SQE 批量提交到内核 SQ ring。
// epoll 和 IOCP 的 I/O 在 await_suspend 中即时发起，无需批处理提交。
void worker_meta::poll_submission() noexcept {
    if (requests_to_submit == 0) [[likely]]
        return;
    int ret = proactor->submit(false);
    if (ret < 0) {
        log::e("[worker] poll_submission failed: %d\n", ret);
    }
    requests_to_submit = 0;
}

// 收割一个 I/O 完成事件。调用 Proactor::wait_completion 获取完成信息，
// 然后通过 handle_completion 解码并处理。
uint32_t worker_meta::poll_completion() noexcept {
    platform::completion_info info{};
    int ret = proactor->wait_completion(&info);
    if (ret > 0) {
        handle_completion(&info);
        return 1;
    }
    return 0;
}

// 处理单个 I/O 完成事件。核心工作流：
//   1. 从 user_data 解码出 task_info 指针
//   2. 保存 I/O 结果到 task_info::result
//   3. 检查链式操作（chain_fn + chain_ctx）：如果第一个操作完成且有链式回调，
//      自动启动第二个操作而不恢复用户协程
//   4. 否则，将用户协程句柄推入 SPSC 环待恢复
//   5. 回收 Proactor 操作对象（IOCP 特有）
void worker_meta::handle_completion(
    const platform::completion_info* info) noexcept
{
    // 减少 inflight 操作计数
    --requests_to_reap;
    log::v("[worker] handle_completion: user_data=%llu result=%d reap=%u\n",
           (unsigned long long)info->user_data, info->result, requests_to_reap);

    // 回收平台操作对象到线程本地空闲链表（ASIO 模式）。
    // 必须在所有 early-return 之前执行，否则 chain_fn 路径和 null-handle
    // 路径会泄漏 iocp_operation，导致 C1000K 压测时内存飙升至 256MB。
    //
    // Recycle the platform operation BEFORE any early-return paths.
    // The chain_fn and null-handle paths must not leak iocp_operation
    // objects — otherwise chained co_await (operator&&) will leak
    // one operation per I/O pair, reaching 256MB at C1000K load.
#if defined(CORONET_PLATFORM_WINDOWS)
    if (info->opaque) {
        auto* raw = static_cast<platform::iocp::iocp_operation*>(info->opaque);
        platform::iocp::recycle_operation(
            std::unique_ptr<platform::iocp::iocp_operation>{raw});
    }
#else
    (void)info->opaque;
#endif

    // 从 user_data 中解码 task_info 指针
    auto* ti = task_info::from_user_data(info->user_data);
    if (!ti) {
        log::w("[worker] handle_completion: null task_info for user_data\n");
        return;
    }

    // 写入 I/O 操作结果（后续 await_resume 返回此值）
    ti->result = info->result;

    // Chained co_await: first op completed → auto-start the second op
    // 链式 co_await 处理（operator&&）：
    //   如果当前 ti 设置了 chain_target，说明是链式操作的第一环。
    //   不恢复用户协程，而是调用第二个 awaiter 内置的 chain_dispatch_fn。
    //   第二个操作完成时才会恢复用户协程。
    //
    //   分发函数指针存储在目标 awaiter 内部（per-type CRTP 静态函数），
    //   编译器在 CRTP 实例化时已知完整 Derived 类型，可生成优化的调用。
    //
    // Chained co_await: first op completed → auto-start the second.
    // Uses the target awaiter's built-in chain_dispatch_fn (set at CRTP
    // instantiation time with complete type info) instead of a void*
    // function pointer pair in task_info.
    if (ti->chain_target) {
#if defined(CORONET_PLATFORM_WINDOWS)
        auto* target = static_cast<win_chain_base*>(ti->chain_target);
        ti->chain_target = nullptr;
        target->chain_issue_next();  // typed dispatch, complete type at compile time
#else
        // epoll path: use epoll_chain_base for typed dispatch
        auto* target = static_cast<epoll_chain_base*>(ti->chain_target);
        ti->chain_target = nullptr;
        target->chain_issue_next();
#endif
        return;
    }

    // 空 handle 可能是链式操作的第一个 SQE（已设置 chain_fn）或
    // io_uring 中通过 IOSQE_IO_LINK 链接的 SQE（内核自动串联）
    if (!ti->handle) {
        // Expected for linked SQEs (first op in chain has null handle)
        log::v("[worker] handle_completion: null handle (linked SQE or chain)\n");
        return;
    }

    // 将用户协程句柄推入 SPSC 环，等待下轮事件循环恢复执行
    log::v("[worker] forwarding task handle=%p\n", ti->handle.address());
    forward_task(ti->handle);

    ti->handle = nullptr;
}

// 检查是否达到批量提交阈值。当前配置为 uint32_t(-1)（实际不启用阈值限制），
// 因为 io_context 的事件循环每轮都会调用 poll_submission。
void worker_meta::check_submission_threshold() noexcept {
    if (requests_to_submit >= config::submission_threshold) {
        poll_submission();
    }
}

} // namespace coronet::detail
