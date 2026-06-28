#pragma once

#ifndef _WIN32

#include <cstdint>
#include <uring/io_uring.h>
#include <uring/utility/kernel_version.hpp>

namespace coronet::config {

inline constexpr unsigned io_uring_setup_flags = 0;

inline constexpr unsigned io_uring_coop_taskrun_flag =
    bool(io_uring_setup_flags & IORING_SETUP_SQPOLL) ? 0
    : (IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG);

inline constexpr uint64_t uring_setup_flags = 0;

/// Use msg_ring for cross-context co_spawn (kernel >= 5.18)
inline constexpr bool is_using_msg_ring = LIBURINGCXX_IS_KERNEL_REACH(5, 18);
inline constexpr bool is_using_eventfd  = !is_using_msg_ring;

/// Timeout flags (aligned with co_context)
inline constexpr uint32_t timeout_flags =
    LIBURINGCXX_IS_KERNEL_REACH(6, 0) ? IORING_TIMEOUT_ETIME_SUCCESS : 0
    | (LIBURINGCXX_IS_KERNEL_REACH(5, 15) ? IORING_TIMEOUT_BOOTTIME : 0);

} // namespace coronet::config

#endif // !_WIN32
