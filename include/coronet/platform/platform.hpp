#pragma once
// ============================================================
// platform.hpp — 平台检测 + 跨平台类型定义
// ============================================================
// 统一抽象层：屏蔽 Windows/Linux 差异。
//   - CORONET_PLATFORM_WINDOWS / CORONET_PLATFORM_LINUX：平台宏
//   - socket_handle_t / file_handle_t：平台句柄类型别名
//   - completion_info：跨平台 I/O 完成结果结构体

// ---- 平台检测 / Platform detection ----
#if defined(_WIN32) || defined(_WIN64)
    #define CORONET_PLATFORM_WINDOWS 1
#elif defined(__linux__)
    #define CORONET_PLATFORM_LINUX 1
#else
    #error "coronet: unsupported platform (only Windows and Linux are supported)"
#endif

// ---- 编译器检测 / Compiler detection (for attributes) ----
#if defined(__clang__)
    #define CORONET_COMPILER_CLANG 1
#elif defined(__GNUC__) || defined(__GNUG__)
    #define CORONET_COMPILER_GCC 1
#elif defined(_MSC_VER)
    #define CORONET_COMPILER_MSVC 1
#endif

#include <cstdint>
#include <cstddef>

// ---- 可移植性垫片 / Portability shims ----

// Windows 用 int 表示地址长度；POSIX 用 socklen_t。
// Windows uses `int` for socket address length; POSIX uses `socklen_t`.
// Define it at global scope so both POSIX system headers and our code agree.
#if defined(_WIN32) && !defined(socklen_t)
typedef int socklen_t;
#endif

// Windows 不定义 sa_family_t（winsock2 的 ADDRESS_FAMILY 是 u_short）
// Windows doesn't define sa_family_t (the ADDRESS_FAMILY from winsock2 is u_short).
#if defined(_WIN32) && !defined(sa_family_t)
typedef unsigned short sa_family_t;
#endif

namespace coronet::platform {

// ---- 平台特定类型别名 / Platform-specific type aliases ----

#if defined(CORONET_PLATFORM_WINDOWS)
    // Windows: SOCKET = UINT_PTR, HANDLE = void*
    using socket_handle_t = uintptr_t;
    using file_handle_t   = void*;
    inline constexpr socket_handle_t invalid_socket = (socket_handle_t)(~0ULL);
    inline constexpr file_handle_t   invalid_file   = nullptr;
#else
    // Linux: 一切都是 int fd
    using socket_handle_t = int;
    using file_handle_t   = int;
    inline constexpr socket_handle_t invalid_socket = -1;
    inline constexpr file_handle_t   invalid_file   = -1;
#endif

// ---- 完成信息 / Completion info ----
// 统一的跨平台 I/O 完成结果。三个 Proactor（io_uring/epoll/IOCP）都将
// 完成事件映射到此结构体，worker_meta::handle_completion 统一处理。
//
// Unified cross-platform I/O completion result.  All three proactors
// map their completion events to this struct.

struct completion_info {
    uint64_t user_data;   // 标识原始操作（通常是 task_info 指针编码）
                          // identifies the original operation (usually encoded task_info ptr)
    int32_t  result;      // 返回码（>= 0 成功，< 0 为负 errno 或 WSA 错误码）
                          // return code (>= 0 success, < 0 error / negative errno or WSA error)
    uint32_t flags;       // 平台特定标志 / platform-specific flags
    void*    opaque;      // 平台特定操作指针（用于回收）/ platform-specific operation pointer (for recycling)
};

} // namespace coronet::platform
