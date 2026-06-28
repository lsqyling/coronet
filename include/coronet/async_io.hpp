/// Async I/O factory functions (cross-platform).
/// Platform dispatch via factory functions in platform_io namespace — C++20 style.
#pragma once

#if defined(CORONET_PLATFORM_LINUX)
#include "coronet/platform/io_uring/io_uring_lazy_io.hpp"
#else
#include "coronet/platform/iocp/iocp_win_io.hpp"
#endif

#include <chrono>
#include <cstdint>
#include <span>

namespace coronet {
inline namespace async {

// ============================================================
// Socket operations
// ============================================================

[[nodiscard("Did you forget to co_await?")]]
inline auto recv(int fd, std::span<char> buf, int flags = 0) noexcept
    { return detail::platform_io::make_recv(fd, buf, flags); }

[[nodiscard("Did you forget to co_await?")]]
inline auto send(int fd, std::span<const char> buf, int flags = 0) noexcept
    { return detail::platform_io::make_send(fd, buf, flags); }

[[nodiscard("Did you forget to co_await?")]]
inline auto accept(int fd, struct sockaddr* addr = nullptr,
                   socklen_t* addrlen = nullptr, int flags = 0) noexcept
    { return detail::platform_io::make_accept(fd, addr, addrlen, flags); }

[[nodiscard("Did you forget to co_await?")]]
inline auto connect(int fd, const struct sockaddr* addr,
                    socklen_t addrlen) noexcept
    { return detail::platform_io::make_connect(fd, addr, addrlen); }

[[nodiscard("Did you forget to co_await?")]]
inline auto close(int fd) noexcept
    { return detail::platform_io::make_close(fd); }

[[nodiscard("Did you forget to co_await?")]]
inline auto shutdown(int fd, int how) noexcept
    { return detail::platform_io::make_shutdown(fd, how); }

// ============================================================
// File I/O
// ============================================================

[[nodiscard("Did you forget to co_await?")]]
inline auto read(int fd, std::span<char> buf,
                 uint64_t offset = uint64_t(-1)) noexcept
    { return detail::platform_io::make_read(fd, buf, offset); }

[[nodiscard("Did you forget to co_await?")]]
inline auto write(int fd, std::span<const char> buf,
                  uint64_t offset = uint64_t(-1)) noexcept
    { return detail::platform_io::make_write(fd, buf, offset); }

// openat is Linux-only (io_uring specific)
#if defined(CORONET_PLATFORM_LINUX)
[[nodiscard("Did you forget to co_await?")]]
inline auto openat(int dirfd, const char* path, int flags,
                   mode_t mode = 0) noexcept {
    return detail::io_uring_openat{dirfd, path, flags, mode};
}
#endif

// ============================================================
// Control / yield / timeout
// ============================================================

[[nodiscard("Did you forget to co_await?")]]
inline auto yield() noexcept
    { return detail::platform_io::make_yield(); }

[[nodiscard("Did you forget to co_await?")]]
inline auto timeout(auto dur) noexcept
    { return detail::platform_io::make_timeout(dur); }

[[nodiscard("Did you forget to co_await?")]]
inline auto timeout_at(auto time_point) noexcept {
    auto dur = std::chrono::duration_cast<std::chrono::nanoseconds>(
        time_point - std::chrono::steady_clock::now());
    if (dur.count() < 0) dur = std::chrono::nanoseconds{0};
    return detail::platform_io::make_timeout(dur);
}

} // namespace async
} // namespace coronet
