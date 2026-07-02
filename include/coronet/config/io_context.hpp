#pragma once
// ============================================================
// config/io_context.hpp — 事件循环编译期可配置参数
// ============================================================
/*
 * 本文件定义 io_context 事件循环所需的所有编译期常量。
 *
 * 为什么使用编译期常量而非运行时配置：
 * - 编译器可以完全内联和常量折叠，消除运行时分支和间接调用
 * - 例如 swap_capacity 为 2 的幂，编译器可将取模运算优化为位运算
 * - 性能关键路径上的每个分支都会影响流水线和分支预测
 *
 * 修改这些常量后需要重新编译整个库，这是性能换灵活性的典型权衡。
 */
// 这些是编译期常量（inline constexpr），不是运行时配置。
// These are compile-time constants (inline constexpr), not runtime config.
// 修改后需重新编译。
// Modify these and rebuild the library.

#include <cstdint>
#include <cstddef>
#include <bit>

namespace coronet::config {

/// CPU 缓存行大小（用于 alignas 避免 false sharing）
/// Cache line size for alignment (avoid false sharing)
/*
 * 缓存行是 CPU 缓存一致性协议（如 MESI）的最小传输单位。
 * 多核场景下，不同核心修改同一缓存行的不同变量会引发"伪共享"（false sharing），
 * 导致缓存行在核心间频繁失效和同步，造成严重的性能退化。
 * 设为 64 字节（x86_64 和大部分 ARM 处理器的缓存行大小），
 * 用于 alignas 确保关键结构体不会跨缓存行，或隔离不同线程访问的变量。
 */
inline constexpr size_t cache_line_size = 64;

/// io_context 标识符类型（最多 255 个上下文）
/// io_context identifier type (max 255 contexts)
/*
 * uint8_t 可表示 256 个不同 io_context，足够应对大多数多核场景。
 * 使用最小整数类型不仅节省内存（在 SPSC 环和其他数据结构中大量出现），
 * 也能在 x86 上实现更紧凑的指令编码。
 * 如果超出 255 个 io_context，只需将这里改为 uint16_t。
 */
using ctx_id_t = uint8_t;

/// SPSC reap_swap 环游标类型
/// Cursor type for SPSC reap_swap ring
/*
 * uint32_t 满足 2^32 的环形空间，远超过实际容量 swap_capacity=16384，
 * 可利用自然溢出实现取模运算的零开销语义。
 * 选择 32 位而非 64 位是因为它在 x86 上更高效（32 位操作不依赖 REX 前缀）。
 */
using cur_t = uint32_t;

/// SPSC 环容量（必须是 2 的幂）
/// Capacity of the reap_swap ring (must be power of two)
/*
 * 16384 是经过实践验证的平衡值：
 * - 足够大以避免生产者过快时消费者来不及消费导致阻塞
 * - 不会占用过多内存（每个槽位仅为一指针大小）
 * - 2 的幂使得取模运算 (idx & (capacity-1)) 替换为昂贵的除法指令
 *
 * 为什么需要 ring：
 * - co_spawn 和完成事件提交都在同一个线程内完成，通过 ring 实现无锁传递
 * - 相比 mutex+vector，SPSC ring 在单生产者/单消费者场景下完全没有锁竞争
 */
inline constexpr cur_t swap_capacity = 16384;

/// io_uring 默认 SQ 条目数（>= 2 * swap_capacity，2 的幂）
/// Default io_uring entries (must be power of two, >= 2 * swap_capacity)
/// 也用作 epoll 的 max_events 参数
/*
 * 使用 std::bit_ceil 自动计算大于等于 2*swap_capacity 的最小 2 的幂。
 * 设置为 swap_capacity 的两倍是因为：
 * - 一部分 SQE 用于新的 I/O 提交
 * - 另一部分留给内核处理中的 I/O 的重提交或链接操作
 * - 避免提交队列满时阻塞用户代码
 * 该值同时也作为 epoll 的 max_events 参数，确保不会丢失事件。
 */
inline constexpr uint32_t default_io_uring_entries =
    std::bit_ceil<uint32_t>(static_cast<uint32_t>(swap_capacity) * 2U);

/// 批量提交阈值（设为最大值表示不限制，每次事件循环都提交）
/// Max bytes to submit in one batch (unlimited = submit every loop iteration)
/*
 * submission_threshold 控制在一次事件循环中提交多少个 I/O 操作。
 * 设置为 -1 (最大值) 表示"不做批量限制"——每次事件循环提交所有待处理操作。
 * 这个设计的考虑：
 * - 无限制可以最小化延迟：每个操作都能立即提交给内核
 * - 批量提交可以减少系统调用次数（每次 io_uring_enter 可提交多个 SQE）
 * - 但如果需要控制延迟波动（jitter），可以设置一个较小的阈值来平衡吞吐和延迟
 */
inline constexpr uint32_t submission_threshold = uint32_t(-1);

/// 信号量计数器类型
/// Semaphore counter type
/*
 * 使用 std::ptrdiff_t 而非 unsigned 类型，原因是 C++ 信号量标准要求
 * 计数器可以为负（表示有等待者），且 max() 需要是有符号类型。
 * ptrdiff_t 在 64 位系统上为 64 位，足以表示任意合理的计数值。
 */
using semaphore_counting_t = std::ptrdiff_t;

/// 超时偏置（纳秒，负值 = 提前触发以补偿处理延迟）
/// Bias applied to timeout to compensate for processing latency (in ns)
/*
 * 网络 I/O 的超时是从内核检测到事件到应用程序感知到的时刻，
 * 期间有处理延迟（中断处理、事件循环调度、协程恢复等）。
 * -30 微秒的负偏置意味着实际超时时间比用户请求的短 30us，
 * 这样即使有处理延迟，用户感知到的超时也更接近预期值。
 * 负偏置是"提前触发"策略，是为了补偿不可避免的处理流水线延迟。
 */
inline constexpr int64_t timeout_bias_nanosecond = -30'000;

} // namespace coronet::config
