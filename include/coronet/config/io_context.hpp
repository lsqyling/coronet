#pragma once

#include <cstdint>
#include <cstddef>
#include <bit>

namespace coronet::config {

/// Cache line size for alignment
inline constexpr size_t cache_line_size = 64;

/// io_context identifier type (max 255 contexts)
using ctx_id_t = uint8_t;

/// Cursor type for SPSC reap_swap ring
using cur_t = uint32_t;

/// Capacity of the reap_swap ring (must be power of two)
inline constexpr cur_t swap_capacity = 16384;

/// Default io_uring entries (must be power of two, >= 2 * swap_capacity)
inline constexpr uint32_t default_io_uring_entries =
    std::bit_ceil<uint32_t>(static_cast<uint32_t>(swap_capacity) * 2U);

/// Max bytes to submit in one batch (all if unlimited)
inline constexpr uint32_t submission_threshold = uint32_t(-1);

/// Semaphore counter type
using semaphore_counting_t = std::ptrdiff_t;

/// Condition variable internal counter type
using condition_variable_counting_t = std::uintptr_t;

/// Bias applied to timeout to compensate for processing latency (in ns)
inline constexpr int64_t timeout_bias_nanosecond = -30'000;

} // namespace coronet::config
