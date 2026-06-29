#pragma once
// ============================================================
// config/io_context.hpp — 事件循环编译期可配置参数
// ============================================================
// 这些是编译期常量（inline constexpr），不是运行时配置。
// 修改后需重新编译。

#include <cstdint>
#include <cstddef>
#include <bit>

namespace coronet::config {

/// CPU 缓存行大小（用于 alignas 避免 false sharing）
/// Cache line size for alignment (avoid false sharing)
inline constexpr size_t cache_line_size = 64;

/// io_context 标识符类型（最多 255 个上下文）
/// io_context identifier type (max 255 contexts)
using ctx_id_t = uint8_t;

/// SPSC reap_swap 环游标类型
/// Cursor type for SPSC reap_swap ring
using cur_t = uint32_t;

/// SPSC 环容量（必须是 2 的幂）
/// Capacity of the reap_swap ring (must be power of two)
inline constexpr cur_t swap_capacity = 16384;

/// io_uring 默认 SQ 条目数（>= 2 * swap_capacity，2 的幂）
/// Default io_uring entries (must be power of two, >= 2 * swap_capacity)
/// 也用作 epoll 的 max_events 参数
inline constexpr uint32_t default_io_uring_entries =
    std::bit_ceil<uint32_t>(static_cast<uint32_t>(swap_capacity) * 2U);

/// 批量提交阈值（设为最大值表示不限制，每次事件循环都提交）
/// Max bytes to submit in one batch (unlimited = submit every loop iteration)
inline constexpr uint32_t submission_threshold = uint32_t(-1);

/// 信号量计数器类型
/// Semaphore counter type
using semaphore_counting_t = std::ptrdiff_t;

/// 超时偏置（纳秒，负值 = 提前触发以补偿处理延迟）
/// Bias applied to timeout to compensate for processing latency (in ns)
inline constexpr int64_t timeout_bias_nanosecond = -30'000;

} // namespace coronet::config
