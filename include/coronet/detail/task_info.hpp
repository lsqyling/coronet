#pragma once

#include <coroutine>
#include <cstdint>

namespace coronet::detail {

/// Per-operation info: stores the awaiting coroutine handle and the I/O result.
/// user_data encoding for completions:
///   - bits [63:3]: pointer to this task_info (8-byte aligned)
///   - bits [2:0] : type tag (reserved for future use)
struct task_info {
    std::coroutine_handle<> handle{nullptr};
    int32_t result{0};
    void*  chain_ctx{nullptr};               // chained: pointer to next operation
    void (*chain_fn)(void* ctx) noexcept {nullptr};  // chained: starts next I/O

    /// Encode this pointer as the user_data for SQE / OVERLAPPED
    uint64_t as_user_data() const noexcept {
        return reinterpret_cast<uintptr_t>(this);
    }

    /// Decode a user_data back to task_info*
    static task_info* from_user_data(uint64_t ud) noexcept {
        return reinterpret_cast<task_info*>(ud & ~uint64_t(7));
    }

    /// Extract type tag from user_data
    static uint8_t type_tag(uint64_t ud) noexcept {
        return static_cast<uint8_t>(ud & 7);
    }
};

} // namespace coronet::detail
