#pragma once

// Platform detection
#if defined(_WIN32) || defined(_WIN64)
    #define CORONET_PLATFORM_WINDOWS 1
#elif defined(__linux__)
    #define CORONET_PLATFORM_LINUX 1
#else
    #error "coronet: unsupported platform (only Windows and Linux are supported)"
#endif

// Compiler detection (for attributes)
#if defined(__clang__)
    #define CORONET_COMPILER_CLANG 1
#elif defined(__GNUC__) || defined(__GNUG__)
    #define CORONET_COMPILER_GCC 1
#elif defined(_MSC_VER)
    #define CORONET_COMPILER_MSVC 1
#endif

#include <cstdint>
#include <cstddef>

// Windows uses `int` for socket address length; POSIX uses `socklen_t`.
// Define it at global scope so both POSIX system headers and our code agree.
#if defined(_WIN32) && !defined(socklen_t)
typedef int socklen_t;
#endif

// Windows doesn't define sa_family_t (the ADDRESS_FAMILY from winsock2 is u_short).
#if defined(_WIN32) && !defined(sa_family_t)
typedef unsigned short sa_family_t;
#endif

namespace coronet::platform {

// Platform-specific type aliases

#if defined(CORONET_PLATFORM_WINDOWS)
    using socket_handle_t = uintptr_t;   // SOCKET is UINT_PTR
    using file_handle_t   = void*;       // HANDLE
    inline constexpr socket_handle_t invalid_socket = (socket_handle_t)(~0ULL);
    inline constexpr file_handle_t   invalid_file   = nullptr;
#else
    using socket_handle_t = int;
    using file_handle_t   = int;
    inline constexpr socket_handle_t invalid_socket = -1;
    inline constexpr file_handle_t   invalid_file   = -1;
#endif

// Completion info – both platforms map to this
struct completion_info {
    uint64_t user_data;   // identifies the original operation
    int32_t  result;      // return code (>= 0 success, < 0 error / negative errno)
    uint32_t flags;       // platform-specific flags
    void*    opaque;      // platform-specific operation pointer (for recycling)
};

} // namespace coronet::platform
