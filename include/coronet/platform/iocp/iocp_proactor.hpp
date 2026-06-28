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
//
// OVERLAPPED is the PRIMARY base class (at offset 0), so:
//  - GQCS returns OVERLAPPED* that IS the operation
//  - static_cast between OVERLAPPED* and iocp_operation* is zero-cost
//
// Follows ASIO's win_iocp_operation pattern.

class iocp_operation : public OVERLAPPED {
public:
    iocp_operation() noexcept : OVERLAPPED{} {}

    // ---- Concept: operation_concept ----
    void set_user_data(uint64_t ud) noexcept { user_data_ = ud; }
    void prepare() noexcept { /* no-op for IOCP */ }
    void cancel() noexcept { /* TODO: CancelIoEx */ }

    uint64_t get_user_data() const noexcept { return user_data_; }

    // Direct OVERLAPPED access (OVERLAPPED is primary base at offset 0)
    OVERLAPPED* native_overlapped() noexcept { return static_cast<OVERLAPPED*>(this); }
    const OVERLAPPED* native_overlapped() const noexcept { return static_cast<const OVERLAPPED*>(this); }

    void set_awaiting_handle(std::coroutine_handle<> h) noexcept { awaiting_handle_ = h; }
    std::coroutine_handle<> awaiting_handle() const noexcept { return awaiting_handle_; }

    // ---- ready_ sync protocol (ASIO pattern) ----
    bool is_ready() noexcept {
        return InterlockedCompareExchange(&ready_, 1, 0) != 0;
    }

    void on_pending(struct iocp_proactor* proactor);
    void on_sync_completion(struct iocp_proactor* proactor, DWORD bytes);

    static iocp_operation* from_overlapped(OVERLAPPED* ov) noexcept {
        return static_cast<iocp_operation*>(ov);
    }

    /// Reset for recycling: clears all fields before reuse.
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

class iocp_proactor {
public:
    using operation_type = iocp_operation;

    iocp_proactor() noexcept = default;
    ~iocp_proactor() noexcept { deinit(); }

    // Non-copyable, non-movable
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
    std::unique_ptr<iocp_operation> acquire_operation();

    void post_completion(iocp_operation* op, DWORD bytes, DWORD key);

    /// Idempotent — call once per socket to associate with this IOCP.
    void register_handle(uintptr_t sock) noexcept;

    // ---- Work tracking ----
    void work_started() noexcept { ++outstanding_work_; }
    void work_finished() noexcept { --outstanding_work_; }
    bool has_outstanding_work() const noexcept {
        return outstanding_work_.load(std::memory_order_acquire) > 0;
    }

    // ---- Batch completion ----
    int poll_completions_impl(void* ctx,
        void (*callback_fn)(void*, const completion_info*)) noexcept;

private:
    void* iocp_handle_ = nullptr;  // HANDLE
    uint32_t entries_ = 0;
    std::atomic<int64_t> outstanding_work_{0};
};

// Free function: recycle an iocp_operation to per-thread free list.
// Takes ownership via unique_ptr&& (no raw new/delete at call sites).
void recycle_operation(std::unique_ptr<iocp_operation> op) noexcept;

} // namespace coronet::platform::iocp
