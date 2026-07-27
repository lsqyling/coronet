#pragma once

// ============================================================
// tcp_socket.hpp — TCP 套接字（面向连接的可靠数据流）
// ============================================================
// 继承 socket_base，添加 TCP 特有操作：
//   - connect (async) — 异步连接到远端
//   - recv / send — 异步收发数据流
//   - listen — 开始监听（服务端）
//   - shutdown_write / shutdown_read / shutdown_both — 半关闭
//   - set_tcp_no_delay / set_keepalive — TCP 特有选项
//
// 满足 transport concept — 可用于泛型传输层代码。
//
// 向后兼容：using socket = tcp_socket;
//   旧代码中使用 socket 的地方自动适配为 tcp_socket。
//   注意：socket::create_tcp → tcp_socket::create_tcp（保留旧名 + 新增 create）

#include "coronet/net/socket_base.hpp"

#include <system_error>

#if defined(CORONET_PLATFORM_WINDOWS)
#include <winsock2.h>
#include <mswsock.h>
#else
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace coronet {

/// TCP 套接字 — 面向连接的可靠数据流传输。
///
/// 满足 transport concept。
///
/// 使用方式：
///   // 客户端
///   tcp_socket sock = tcp_socket::create_tcp(AF_INET);
///   co_await sock.connect(addr);
///   co_await sock.send(data);
///   int n = co_await sock.recv(buf);
///
///   // 服务端（通过 tcp_acceptor）
///   tcp_acceptor ac{addr};
///   tcp_socket conn = co_await ac.accept_socket();
class tcp_socket : public socket_base<tcp_socket> {
public:
    /// 从 fd 构造（public — 供工厂方法、acceptor、外部代码使用）
    explicit tcp_socket(int fd) noexcept : socket_base<tcp_socket>(fd) {}

    // 移动语义（默认 — 调用基类的移动操作）
    tcp_socket(tcp_socket&&) noexcept = default;
    tcp_socket& operator=(tcp_socket&&) noexcept = default;

    // ---- TCP 特有同步操作 ----

    /// 开始监听传入连接
    tcp_socket& listen(int backlog = SOMAXCONN);
    /// 设置 TCP_NODELAY — 禁用 Nagle 算法
    tcp_socket& set_tcp_no_delay(bool on);
    /// 设置 SO_KEEPALIVE — 启用 TCP 保活
    tcp_socket& set_keepalive(bool on);

    // ---- TCP 特有异步操作 ----

    /// 异步连接到远端地址
    [[nodiscard("Did you forget to co_await?")]]
    auto connect(const inet_address& addr) noexcept {
        return async::connect((int)sockfd_, addr.get_sockaddr(), addr.length());
    }

    /// 异步接收数据
    [[nodiscard("Did you forget to co_await?")]]
    auto recv(std::span<char> buf, int flags = 0) noexcept {
        return async::recv((int)sockfd_, buf, flags);
    }

    /// 异步发送数据
    [[nodiscard("Did you forget to co_await?")]]
    auto send(std::span<const char> buf, int flags = 0) noexcept {
        return async::send((int)sockfd_, buf, flags);
    }

    /// 关闭写端（半关闭）— 通知对端数据发送完毕
    [[nodiscard("Did you forget to co_await?")]]
    auto shutdown_write() noexcept {
#if defined(CORONET_PLATFORM_WINDOWS)
        return async::shutdown((int)sockfd_, SD_SEND);
#else
        return async::shutdown((int)sockfd_, SHUT_WR);
#endif
    }

    /// 关闭读端
    [[nodiscard("Did you forget to co_await?")]]
    auto shutdown_read() noexcept {
#if defined(CORONET_PLATFORM_WINDOWS)
        return async::shutdown((int)sockfd_, SD_RECEIVE);
#else
        return async::shutdown((int)sockfd_, SHUT_RD);
#endif
    }

    /// 关闭读写两端
    [[nodiscard("Did you forget to co_await?")]]
    auto shutdown_both() noexcept {
#if defined(CORONET_PLATFORM_WINDOWS)
        return async::shutdown((int)sockfd_, SD_BOTH);
#else
        return async::shutdown((int)sockfd_, SHUT_RDWR);
#endif
    }

    // ---- 工厂方法 ----

    /// 创建非阻塞 TCP 套接字
    static tcp_socket create_tcp(sa_family_t family);
    /// create 的别名（更简洁的命名）
    static tcp_socket create(sa_family_t family) { return create_tcp(family); }
};

// ---- 内联实现 ----

inline tcp_socket tcp_socket::create_tcp(sa_family_t family) {
#if defined(CORONET_PLATFORM_LINUX)
    int fd = ::socket(family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_TCP);
#else
    int fd = (int)::WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                               WSA_FLAG_OVERLAPPED);
#endif
    if (fd < 0)
        throw std::system_error(
#if defined(CORONET_PLATFORM_WINDOWS)
            WSAGetLastError(), std::system_category(), "create_tcp"
#else
            errno, std::generic_category(), "create_tcp"
#endif
        );
    return tcp_socket{fd};
}

/// 向后兼容别名 — 旧代码中的 socket 映射到 tcp_socket
using socket = tcp_socket;

} // namespace coronet
