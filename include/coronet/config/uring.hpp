#pragma once

#ifndef _WIN32

#include <cstdint>
#include <uring/io_uring.h>
#include <uring/utility/kernel_version.hpp>

namespace coronet::config {

/*
 * io_uring 的设置标志，影响内核端 SQ 线程的工作方式。
 * 当前为 0，表示使用默认的"应用线程主动提交"模式。
 *
 * 如果启用 IORING_SETUP_SQPOLL，内核会创建一个内核线程轮询 SQ，
 * 应用无需每次执行系统调用。但 SQPOLL 模式会增加额外延迟和复杂度，
 * 且对大多数工作负载来说收益有限——因为 coronet 的事件循环本身就是
 * 一个高效的轮询器。保持为 0 是一种"保持简单"的设计选择。
 */
inline constexpr unsigned io_uring_setup_flags = 0;

/*
 * IORING_SETUP_COOP_TASKRUN 和 IORING_SETUP_TASKRUN_FLAG 是
 * Linux 5.17+ 引入的特性，用于优化 io_uring 的 task_work 通知机制。
 *
 * 当启用时，内核会尽量在用户线程主动进入内核时顺便处理完成事件，
 * 而非异步发送 IPI 中断。这减少了上下文切换和核间中断。
 *
 * 仅在未设置 SQPOLL 时才启用这些标志，因为 SQPOLL 有自己的线程模型，
 * 与 COOP_TASKRUN 的语义不完全兼容。这里使用条件编译（SQPOLL 为 0，
 * 所以实际总是启用），保持了设计上的正确性。
 */
inline constexpr unsigned io_uring_coop_taskrun_flag =
    bool(io_uring_setup_flags & IORING_SETUP_SQPOLL) ? 0
    : (IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG);

inline constexpr uint64_t uring_setup_flags = 0;

/// Use msg_ring for cross-context co_spawn (kernel >= 5.18)
/*
 * IORING_OP_MSG_RING 是 Linux 5.18 引入的操作，允许一个 io_uring 实例
 * 向另一个 io_uring 实例发送消息。这为跨 io_context 的协程调度提供了
 * 高效的机制——不需要额外的 eventfd 和系统调用。
 *
 * 为什么优先使用 msg_ring 而非 eventfd：
 * - eventfd 方案：先往 eventfd 写数据唤醒目标线程，目标线程从 eventfd 读取
 *   后才知道有任务到来。这需要两次系统调用和一次额外的上下文切换。
 * - msg_ring 方案：直接通过 io_uring 的完成队列通知目标线程，零额外开销。
 *
 * 通过 LIBURINGCXX_IS_KERNEL_REACH 宏在编译期检测内核版本，
 * 自动选择最优方案。运行时的内核版本检查由 liburing 库处理。
 */
inline constexpr bool is_using_msg_ring = LIBURINGCXX_IS_KERNEL_REACH(5, 18);
inline constexpr bool is_using_eventfd  = !is_using_msg_ring;

/// Timeout flags (aligned with co_context)
/*
 * IORING_TIMEOUT_ETIME_SUCCESS（内核 6.0+）：
 * - 默认行为：超时返回 -ETIME 错误码
 * - 启用后：超时返回 0（成功），使超时行为与标准 POSIX 定时器一致
 * - 这样可以简化协程代码——超时被视为正常完成而非异常
 *
 * IORING_TIMEOUT_BOOTTIME（内核 5.15+）：
 * - 使用 CLOCK_BOOTTIME 而非默认的 CLOCK_MONOTONIC
 * - CLOCK_BOOTTIME 包含系统挂起的时间，适用于"墙上时间"敏感的场景
 * - 如果用户希望超时在系统挂起时也持续计时，这是正确选择
 */
inline constexpr uint32_t timeout_flags =
    LIBURINGCXX_IS_KERNEL_REACH(6, 0) ? IORING_TIMEOUT_ETIME_SUCCESS : 0
    | (LIBURINGCXX_IS_KERNEL_REACH(5, 15) ? IORING_TIMEOUT_BOOTTIME : 0);

} // namespace coronet::config

#endif // !_WIN32
