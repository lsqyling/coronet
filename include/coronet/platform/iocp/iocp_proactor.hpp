#pragma once

#include "coronet/platform/platform.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>

#include <coroutine>
#include <cstdint>
#include <atomic>
#include <memory>

namespace coronet::platform::iocp {

// ============================================================
// iocp_operation — IOCP overlapped operation (standalone, no virtual base)
// ============================================================
// iocp_operation — IOCP 重叠操作（无虚函数，独立类型）
//
// OVERLAPPED is the PRIMARY base class (at offset 0), so:
//  - GQCS returns OVERLAPPED* that IS the operation
//  - static_cast between OVERLAPPED* and iocp_operation* is zero-cost
//
// OVERLAPPED 作为主基类（位于偏移 0 处），因此：
//  - GetQueuedCompletionStatus 返回的 OVERLAPPED* 就是 iocp_operation* 本身
//  - OVERLAPPED* 与 iocp_operation* 之间的 static_cast 是零成本的（地址值不变）
//
// 这是 Windows IOCP 的关键设计模式：I/O 操作的生命周期由 OVERLAPPED 结构承载，
// 内核通过 OVERLAPPED 指针返回完成事件，而我们的扩展字段（user_data, awaiting_handle 等）
// 通过继承 OVERLAPPED 自然获得，无需额外的映射表或查找。
//
// Follows ASIO's win_iocp_operation pattern.
// 遵循 ASIO 的 win_iocp_operation 设计模式
//
// 同步协议（ready_ flag）：
//   Windows 的 I/O API 可以同步完成（立即返回）或异步完成（返回 WSA_IO_PENDING）。
//   同步完成时，我们需要手动将结果 post 到 IOCP；异步完成时，内核会自动 post。
//   ready_ flag 用于区分这两种情况，避免重复处理同一个完成事件。
//   - on_pending：          操作确实异步，标记 ready_ 为 1 等待内核 post
//   - on_sync_completion：  操作同步完成，手动设置结果并 post 到 IOCP

class iocp_operation : public OVERLAPPED {
public:
    iocp_operation() noexcept : OVERLAPPED{} {}

    // ---- Concept: operation_concept ----
    void set_user_data(uint64_t ud) noexcept { user_data_ = ud; }
    void prepare() noexcept { /* no-op for IOCP */ }
    void cancel() noexcept { /* TODO: CancelIoEx */ }

    uint64_t get_user_data() const noexcept { return user_data_; }

    // Direct OVERLAPPED access (OVERLAPPED is primary base at offset 0)
    // 直接获取 OVERLAPPED 指针（OVERLAPPED 是主基类，位于偏移 0）
    OVERLAPPED* native_overlapped() noexcept { return static_cast<OVERLAPPED*>(this); }
    const OVERLAPPED* native_overlapped() const noexcept { return static_cast<const OVERLAPPED*>(this); }

    void set_awaiting_handle(std::coroutine_handle<> h) noexcept { awaiting_handle_ = h; }
    std::coroutine_handle<> awaiting_handle() const noexcept { return awaiting_handle_; }

    // ---- ready_ sync protocol (ASIO pattern) ----
    // ready_ 同步协议（ASIO 模式）：
    //   使用 InterlockedCompareExchange 原子操作检测操作是否已完成。
    //   如果 ready_ 从 0 变为 1，说明操作已完成，is_ready() 返回 true。
    //   适用于在 IOCP 完成回调中安全地检测操作状态。
    bool is_ready() noexcept {
        return InterlockedCompareExchange(&ready_, 1, 0) != 0;
    }

    void on_pending(struct iocp_proactor* proactor);
    void on_sync_completion(struct iocp_proactor* proactor, DWORD bytes);

    static iocp_operation* from_overlapped(OVERLAPPED* ov) noexcept {
        return static_cast<iocp_operation*>(ov);
    }

    /// Reset for recycling: clears all fields before reuse.
    // 重置以便回收复用：清除所有字段以备再次使用。
    //
    // 为什么要回收 iocp_operation？
    //   在高频 I/O 场景下，每次操作都进行堆分配（new/delete）会产生巨大的性能开销。
    //   通过 per-thread 回收链表（thread_local op_free_list），可以将用过的 operation
    //   放回空闲列表，下次分配时直接复用，避免堆分配。这是 ASIO 采用的经典优化模式。
    void reset_for_reuse() noexcept {
        ::new (static_cast<OVERLAPPED*>(this)) OVERLAPPED{};
        user_data_ = 0;
        ready_ = 0;
    }

private:
    friend class iocp_proactor;
    uint64_t user_data_ = 0;
    std::coroutine_handle<> awaiting_handle_;
    volatile LONG ready_ = 0;
};

// ============================================================
// iocp_proactor — IOCP completion port (standalone, no virtual base)
// ============================================================
// iocp_proactor — IOCP 完成端口（无虚函数，独立类型）
//
// IOCP（I/O Completion Port）是 Windows 内核提供的异步 I/O 框架：
//   - 本质是一个内核管理的完成队列，关联的 socket/file handle 上的异步操作完成时，
//     内核自动将完成事件（携带 OVERLAPPED*）投递到该队列。
//   - 多个工作线程可以同时调用 GetQueuedCompletionStatus 从队列中取出完成事件，
//     内核负责线程间的负载均衡（内核级线程池调度）。
//   - 与 io_uring 不同，IOCP 不提供"提交侧"的统一接口：
//     提交 I/O 操作是直接调用 WSASend/WSARecv 等 API 传入 OVERLAPPED 结构。
//     因此 submit() 方法在这里是空操作（返回 0）。
//
// 关键设计：
//   1. outstanding_work_：跟踪尚未完成的 I/O 操作数量，用于判断事件循环是否可以退出
//   2. PostQueuedCompletionStatus：用于跨线程唤醒和主动投递完成事件
//   3. CreateIoCompletionPort：将 socket handle 关联到 IOCP

class iocp_proactor {
public:
    using operation_type = iocp_operation;

    iocp_proactor() noexcept = default;
    ~iocp_proactor() noexcept { deinit(); }

    // Non-copyable, non-movable
    // 不可拷贝、不可移动（独占 IOCP handle）
    iocp_proactor(const iocp_proactor&) = delete;
    iocp_proactor& operator=(const iocp_proactor&) = delete;
    iocp_proactor(iocp_proactor&&) = delete;
    iocp_proactor& operator=(iocp_proactor&&) = delete;

    // ---- Concept: proactor_concept ----
    void init(uint32_t entries);
    void deinit() noexcept;
    int  submit(bool wait = false) noexcept;
    int  wait_completion(completion_info* info) noexcept;
    intptr_t native_handle() const noexcept;
    void wakeup() noexcept;

    /// Allocate or recycle an operation. Returns owning unique_ptr.
    // 分配或回收一个操作对象。返回拥有所有权的 unique_ptr。
    std::unique_ptr<iocp_operation> acquire_operation();

    void post_completion(iocp_operation* op, DWORD bytes, DWORD key);

    /// Idempotent — call once per socket to associate with this IOCP.
    // 幂等操作 — 每个 socket 调用一次以关联到本 IOCP。
    void register_handle(uintptr_t sock) noexcept;

    // ---- Work tracking ----
    // ---- 工作跟踪 ----
    void work_started() noexcept { ++outstanding_work_; }
    void work_finished() noexcept { --outstanding_work_; }
    bool has_outstanding_work() const noexcept {
        return outstanding_work_.load(std::memory_order_acquire) > 0;
    }

    // ---- Batch completion ----
    // ---- 批量完成收割 ----
    int poll_completions_impl(void* ctx,
        void (*callback_fn)(void*, const completion_info*)) noexcept;

private:
    void* iocp_handle_ = nullptr;  // HANDLE
    uint32_t entries_ = 0;
    std::atomic<int64_t> outstanding_work_{0};
    // outstanding_work_ 跟踪飞行中的操作数量：
    //   - work_started() 在发起操作时调用（+1）
    //   - work_finished() 在操作完成时调用（-1）
    //   - 当 outstanding_work_ 降为 0 时，事件循环可以安全退出
    // 使用 std::atomic 保证跨线程安全性
};

// Free function: recycle an iocp_operation to per-thread free list.
// Takes ownership via unique_ptr&& (no raw new/delete at call sites).
// 自由函数：将 iocp_operation 归还到线程本地空闲链表。
// 通过 unique_ptr&& 传递所有权（调用点无需裸 new/delete）。
void recycle_operation(std::unique_ptr<iocp_operation> op) noexcept;

} // namespace coronet::platform::iocp
