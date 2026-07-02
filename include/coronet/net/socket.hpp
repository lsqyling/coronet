#pragma once
// ============================================================
// socket.hpp — 跨平台 TCP/UDP 套接字 RAII 封装
// ============================================================
// 移动语义（唯一所有权），析构时自动关闭。
// 可移植构造函数 socket{int} 隐藏平台差异：
//   Linux:   socket_handle_t == int，零转换开销
//   Windows: int → uintptr_t (SOCKET)
//
// 异步操作返回 awaitable，标记 [[nodiscard]] 防止忘记 co_await。

// 跨平台套接字 RAII 封装。
//
// ## 为什么需要自定义 socket 类型？
// 原生套接字（POSIX int / Windows SOCKET）是裸资源，容易泄露。
// 通过 RAII 封装，socket 在析构时自动关闭，确保异常安全。
// 移动语义允许在协程间安全转移套接字所有权。
//
// ## 跨平台设计
// 平台内部使用 platform::socket_handle_t 存储句柄，
// 但所有公开接口接受/返回 int（类似 POSIX 风格），
// 在构造函数中完成到平台类型的隐式转换。
// Linux 上 socket_handle_t == int，转换是零开销的。
// Windows 上 int → SOCKET (uintptr_t) 也同样是无运行时开销的。
//
// ## close() 的特殊设计
// close() 先将 sockfd_ 置为 invalid_socket，再发起异步关闭操作。
// 这防止了双重关闭：即使 co_await 被取消或抛出异常，
// 析构函数看到 invalid_socket 后不会再次关闭。
// 这是一种"移交所有权给异步操作"的策略。

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

/// 跨平台套接字 — RAII，移动语义，不可拷贝。
/// Cross-platform socket — RAII, move-only.
class socket {
public:
    /// 可移植构造函数 — 接受 int（类似 POSIX），内部转换为平台句柄。
    /// Portable constructor — accepts int (like POSIX), converts to platform handle.
    /// Linux:   socket_handle_t == int, no conversion needed.
    /// Windows: int → uintptr_t (SOCKET), hiding platform difference.
    explicit socket(int fd) noexcept
        : sockfd_(static_cast<platform::socket_handle_t>(fd)) {
        assert(fd >= 0);
    }

    /// 析构时自动关闭套接字 / Auto-close on destruction
    /// 如果 close() 已提前调用（sockfd_ 已被置为 invalid_socket），则跳过。
    /// 这是 RAII 模式的核心：资源生命周期与对象绑定。
    ~socket() noexcept {
        if (sockfd_ == platform::invalid_socket) return;
#if defined(CORONET_PLATFORM_WINDOWS)
        ::closesocket(static_cast<SOCKET>(sockfd_));
#else
        ::close(static_cast<int>(sockfd_));
#endif
        sockfd_ = platform::invalid_socket;
    }

    // 移动语义 / Move semantics
    // 移动后源对象变为空（持有 invalid_socket），析构空操作。
    // 这保证了套接字唯一所有权语义 —— 避免双重关闭。
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

    // 不可拷贝 / Non-copyable
    socket(const socket&) = delete;
    socket& operator=(const socket&) = delete;

    void swap(socket& other) noexcept { std::swap(sockfd_, other.sockfd_); }

    /// 获取平台原生句柄 / Get the platform native handle
    [[nodiscard]] platform::socket_handle_t native_handle() const noexcept { return sockfd_; }

    // ---- 同步操作 / synchronous operations ----

    /// 绑定地址 / Bind to address
    socket& bind(const inet_address& addr);
    /// 开始监听 / Start listening
    socket& listen(int backlog = SOMAXCONN);
    /// 设置 SO_REUSEADDR / Set address reuse
    socket& set_reuse_addr(bool on);
    /// 设置 TCP_NODELAY / Disable Nagle's algorithm
    socket& set_tcp_no_delay(bool on);
    /// 设为非阻塞模式 / Set non-blocking mode
    void set_nonblocking();

    /// 获取本地地址 / Get local address
    [[nodiscard]] inet_address local_addr() const;
    /// 获取对端地址 / Get peer address
    [[nodiscard]] inet_address peer_addr() const;

    // ---- 异步操作 / async operations (return awaitables) ----

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

    /// 异步关闭：先置 invalid 再 co_await，防止双重关闭
    /// Async close: invalidate handle first, then co_await to prevent double-close.
    ///
    /// ## 为什么需要这种模式？
    /// 如果 co_await 异步操作被取消（或抛出异常），套接字的释放由 RAII 析构函数处理。
    /// 但由于 sockfd_ 已被置为 invalid_socket，析构函数不会再次关闭。
    /// 这避免了双重关闭 UB（未定义行为）。
    ///
    /// 如果异步操作成功完成，内核（io_uring/IOCP）负责在操作完成时关闭套接字。
    [[nodiscard("Did you forget to co_await?")]]
    auto close() noexcept {
        auto fd = sockfd_;
        sockfd_ = platform::invalid_socket;
        return async::close((int)fd);
    }

    /// 关闭写端（半关闭）/ Shutdown write side (half-close)
    /// 适用于 TCP 的半关闭场景：通知对端数据发送完毕，
    /// 但仍可接收数据。常用于 HTTP 等应用层协议。
    [[nodiscard("Did you forget to co_await?")]]
    auto shutdown_write() noexcept {
#if defined(CORONET_PLATFORM_WINDOWS)
        return async::shutdown((int)sockfd_, SD_SEND);
#else
        return async::shutdown((int)sockfd_, SHUT_WR);
#endif
    }

    // ---- 工厂方法 / factory methods ----

    /// 创建非阻塞 TCP 套接字 / Create a non-blocking TCP socket
    static socket create_tcp(sa_family_t family);
    /// 创建非阻塞 UDP 套接字 / Create a non-blocking UDP socket
    static socket create_udp(sa_family_t family);

private:
    platform::socket_handle_t sockfd_;
};

// ---- 内联实现 / inline implementations ----

inline socket socket::create_tcp(sa_family_t family) {
#if defined(CORONET_PLATFORM_LINUX)
    // Linux: 一次性设置 SOCK_CLOEXEC | SOCK_NONBLOCK，避免额外系统调用
    // SOCK_CLOEXEC: exec 时自动关闭，防止泄露到子进程
    // SOCK_NONBLOCK: 初始即为非阻塞模式
    int fd = ::socket(family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_TCP);
#else
    // Windows: WSASocket with WSA_FLAG_OVERLAPPED for IOCP
    // WSA_FLAG_OVERLAPPED 标志是 IOCP 使用的前提条件，
    // 无此标志的套接字无法关联到完成端口。
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
