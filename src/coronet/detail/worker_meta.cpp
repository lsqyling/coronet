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
        std::fprintf(stderr, "worker_meta: reap_swap overflow!\n");
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
    // 如果当前操作设置了 chain_fn，说明这是一个链式操作的第一环。
    // 不恢复用户协程，而是调用 chain_fn 启动后续操作。
    // 第二个操作完成时才会恢复用户协程。
    if (ti->chain_fn && ti->chain_ctx) {
        auto fn = ti->chain_fn;
        auto* ctx = ti->chain_ctx;
        ti->chain_fn = nullptr;
        ti->chain_ctx = nullptr;
        fn(ctx);  // start second I/O (its handle leads to user coroutine)
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

    // 回收平台操作对象：IOCP 使用线程本地空闲链表（ASIO 模式）
    // Recycle the platform operation via unique_ptr (no raw delete)
#if defined(CORONET_PLATFORM_WINDOWS)
    if (info->opaque) {
        auto* raw = static_cast<platform::iocp::iocp_operation*>(info->opaque);
        platform::iocp::recycle_operation(std::unique_ptr<platform::iocp::iocp_operation>{raw});
    }
#else
    (void)info->opaque;
#endif
}

// 检查是否达到批量提交阈值。当前配置为 uint32_t(-1)（实际不启用阈值限制），
// 因为 io_context 的事件循环每轮都会调用 poll_submission。
void worker_meta::check_submission_threshold() noexcept {
    if (requests_to_submit >= config::submission_threshold) {
        poll_submission();
    }
}

} // namespace coronet::detail
