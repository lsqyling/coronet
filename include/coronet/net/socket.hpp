#pragma once

#include "coronet/async_io.hpp"
#include "coronet/net/inet_address.hpp"
#include "coronet/platform/platform.hpp"

#include <cassert>

#if defined(CORONET_PLATFORM_WINDOWS)
#include <winsock2.h>
#include <mswsock.h>
#else
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace coronet {

class socket {
public:
    /// Portable constructor — accepts int (like POSIX), converts to platform handle.
    /// On Linux: socket_handle_t == int, no conversion needed.
    /// On Windows: int → uintptr_t (SOCKET), hiding platform difference.
    explicit socket(int fd) noexcept
        : sockfd_(static_cast<platform::socket_handle_t>(fd)) {
        assert(fd >= 0);
    }

    ~socket() noexcept {
        if (sockfd_ == platform::invalid_socket) return;
#if defined(CORONET_PLATFORM_WINDOWS)
        ::closesocket(static_cast<SOCKET>(sockfd_));
#else
        ::close(static_cast<int>(sockfd_));
#endif
        sockfd_ = platform::invalid_socket;
    }

    socket(socket&& other) noexcept : sockfd_(other.sockfd_) {
        other.sockfd_ = platform::invalid_socket;
    }

    socket& operator=(socket&& other) noexcept {
        if (this != &other) {
            sockfd_ = other.sockfd_;
            other.sockfd_ = platform::invalid_socket;
        }
        return *this;
    }

    socket(const socket&) = delete;
    socket& operator=(const socket&) = delete;

    void swap(socket& other) noexcept {
        std::swap(sockfd_, other.sockfd_);
    }

    [[nodiscard]] platform::socket_handle_t native_handle() const noexcept { return sockfd_; }

    // ---- synchronous operations ----

    socket& bind(const inet_address& addr);
    socket& listen(int backlog = SOMAXCONN);
    socket& set_reuse_addr(bool on);
    socket& set_tcp_no_delay(bool on);
    void set_nonblocking();

    [[nodiscard]] inet_address local_addr() const;
    [[nodiscard]] inet_address peer_addr() const;

    // ---- async operations (return awaitables) ----

    [[nodiscard("Did you forget to co_await?")]]
    auto recv(std::span<char> buf, int flags = 0) noexcept {
        return async::recv((int)sockfd_, buf, flags);
    }

    [[nodiscard("Did you forget to co_await?")]]
    auto send(std::span<const char> buf, int flags = 0) noexcept {
        return async::send((int)sockfd_, buf, flags);
    }

    [[nodiscard("Did you forget to co_await?")]]
    auto connect(const inet_address& addr) noexcept {
        return async::connect((int)sockfd_, addr.get_sockaddr(), addr.length());
    }

    [[nodiscard("Did you forget to co_await?")]]
    auto close() noexcept {
        auto fd = sockfd_;
        sockfd_ = platform::invalid_socket;
        return async::close((int)fd);
    }

    [[nodiscard("Did you forget to co_await?")]]
    auto shutdown_write() noexcept {
#if defined(CORONET_PLATFORM_WINDOWS)
        return async::shutdown((int)sockfd_, SD_SEND);
#else
        return async::shutdown((int)sockfd_, SHUT_WR);
#endif
    }

    // ---- factory methods ----
    static socket create_tcp(sa_family_t family);
    static socket create_udp(sa_family_t family);

private:
    platform::socket_handle_t sockfd_;
};

// ---- inline implementations ----

inline socket socket::create_tcp(sa_family_t family) {
#if defined(CORONET_PLATFORM_LINUX)
    int fd = ::socket(family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_TCP);
#else
    // Windows: WSASocket with WSA_FLAG_OVERLAPPED
    int fd = (int)::WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                               WSA_FLAG_OVERLAPPED);
#endif
    assert(fd >= 0);
    return socket{fd};
}

inline socket socket::create_udp(sa_family_t family) {
#if defined(CORONET_PLATFORM_LINUX)
    int fd = ::socket(family, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_UDP);
#else
    int fd = (int)::WSASocketW(family, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0,
                               WSA_FLAG_OVERLAPPED);
#endif
    assert(fd >= 0);
    return socket{fd};
}

} // namespace coronet
