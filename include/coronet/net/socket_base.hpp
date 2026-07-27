#pragma once

// ============================================================
// socket_base.hpp — 套接字基类（CRTP, RAII + 公共选项）
// ============================================================
// 从 socket.hpp 提取的公共部分，供 tcp_socket 和 udp_socket 继承。
//
// 使用 CRTP (Curiously Recurring Template Pattern) 实现静态多态：
//   - 链式方法返回 Derived& 而非 socket_base&，支持方法链
//   - 零虚表开销，符合项目"静态多态"哲学
//   - tcp_socket : public socket_base<tcp_socket>
//   - udp_socket : public socket_base<udp_socket>
//
// 职责：
//   1. RAII 套接字句柄管理（构造、析构、移动、swap）
//   2. 公共套接字选项（set_reuse_addr / set_reuse_port / 缓冲区 / linger / nonblocking）
//   3. 地址查询（local_addr / peer_addr）
//   4. 异步关闭（close）
//   5. 同步绑定（bind）— TCP 和 UDP 共用
//
// 不包含：
//   - TCP 特有操作（listen / connect / recv / send / shutdown_*）→ tcp_socket
//   - UDP 特有操作（recvfrom / sendto / multicast）→ udp_socket
//   - 工厂方法（create_tcp / create_udp）→ 各派生类

#include "coronet/async_io.hpp"
#include "coronet/net/inet_address.hpp"
#include "coronet/platform/platform.hpp"

#include <optional>
#include <system_error>

#if defined(CORONET_PLATFORM_WINDOWS)
#include <winsock2.h>
#include <mswsock.h>
#else
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace coronet {

/// 套接字基类 — CRTP 模板，RAII 句柄管理 + 公共套接字选项。
///
/// 通过 CRTP 实现链式方法返回 Derived&，支持：
///   sock.set_reuse_addr(true).bind(addr).listen(10);  // tcp_socket
///   sock.set_broadcast(true).bind(addr);               // udp_socket
///
/// 不直接使用 — 通过 tcp_socket 或 udp_socket 创建。
template<typename Derived>
class socket_base {
public:
    /// 析构时自动关闭套接字。
    ~socket_base() noexcept {
        if (sockfd_ == platform::invalid_socket) return;
#if defined(CORONET_PLATFORM_WINDOWS)
        ::closesocket(static_cast<SOCKET>(sockfd_));
#else
        ::close(static_cast<int>(sockfd_));
#endif
        sockfd_ = platform::invalid_socket;
    }

    // 移动语义 — 移动后源对象为空（invalid_socket）
    socket_base(socket_base&& other) noexcept : sockfd_(other.sockfd_) {
        other.sockfd_ = platform::invalid_socket;
    }

    socket_base& operator=(socket_base&& other) noexcept {
        if (this != &other) {
            socket_base tmp(std::move(other));
            swap(tmp);
        }
        return *this;
    }

    // 不可拷贝
    socket_base(const socket_base&) = delete;
    socket_base& operator=(const socket_base&) = delete;

    void swap(socket_base& other) noexcept { std::swap(sockfd_, other.sockfd_); }

    /// 获取平台原生句柄
    [[nodiscard]] platform::socket_handle_t native_handle() const noexcept { return sockfd_; }

    // ---- 公共同步操作（链式，返回 Derived&）----

    /// 绑定地址（TCP 和 UDP 共用）
    Derived& bind(const inet_address& addr) {
        int ret = ::bind((int)sockfd_, addr.get_sockaddr(), addr.length());
        if (ret < 0) {
            throw std::system_error(
#if defined(CORONET_PLATFORM_WINDOWS)
                WSAGetLastError(), std::system_category(), "socket_base::bind"
#else
                errno, std::generic_category(), "socket_base::bind"
#endif
            );
        }
        return static_cast<Derived&>(*this);
    }

    /// 设置 SO_REUSEADDR — 允许重用 TIME_WAIT 状态的地址
    Derived& set_reuse_addr(bool on) {
        int optval = on ? 1 : 0;
        int ret = ::setsockopt((int)sockfd_, SOL_SOCKET, SO_REUSEADDR,
                               (const char*)&optval, sizeof(optval));
        if (ret < 0) {
            throw std::system_error(
#if defined(CORONET_PLATFORM_WINDOWS)
                WSAGetLastError(), std::system_category(), "socket_base::set_reuse_addr"
#else
                errno, std::generic_category(), "socket_base::set_reuse_addr"
#endif
            );
        }
        return static_cast<Derived&>(*this);
    }

    /// 设置 SO_REUSEPORT（Linux 内核负载均衡，Windows 回退 SO_REUSEADDR）
    Derived& set_reuse_port(bool on) {
        int optval = on ? 1 : 0;
#if defined(CORONET_PLATFORM_WINDOWS)
        int ret = ::setsockopt((int)sockfd_, SOL_SOCKET, SO_REUSEADDR,
                               (const char*)&optval, sizeof(optval));
#else
        int ret = ::setsockopt((int)sockfd_, SOL_SOCKET, SO_REUSEPORT,
                               (const char*)&optval, sizeof(optval));
#endif
        if (ret < 0) {
            throw std::system_error(
#if defined(CORONET_PLATFORM_WINDOWS)
                WSAGetLastError(), std::system_category(), "socket_base::set_reuse_port"
#else
                errno, std::generic_category(), "socket_base::set_reuse_port"
#endif
            );
        }
        return static_cast<Derived&>(*this);
    }

    /// 设置 SO_RCVBUF — 接收缓冲区大小
    Derived& set_recv_buffer_size(int size) {
        int ret = ::setsockopt((int)sockfd_, SOL_SOCKET, SO_RCVBUF,
                               (const char*)&size, sizeof(size));
        if (ret < 0) {
            throw std::system_error(
#if defined(CORONET_PLATFORM_WINDOWS)
                WSAGetLastError(), std::system_category(), "socket_base::set_recv_buffer_size"
#else
                errno, std::generic_category(), "socket_base::set_recv_buffer_size"
#endif
            );
        }
        return static_cast<Derived&>(*this);
    }

    /// 设置 SO_SNDBUF — 发送缓冲区大小
    Derived& set_send_buffer_size(int size) {
        int ret = ::setsockopt((int)sockfd_, SOL_SOCKET, SO_SNDBUF,
                               (const char*)&size, sizeof(size));
        if (ret < 0) {
            throw std::system_error(
#if defined(CORONET_PLATFORM_WINDOWS)
                WSAGetLastError(), std::system_category(), "socket_base::set_send_buffer_size"
#else
                errno, std::generic_category(), "socket_base::set_send_buffer_size"
#endif
            );
        }
        return static_cast<Derived&>(*this);
    }

    /// 设置 SO_LINGER — 控制套接字关闭时的等待行为
    Derived& set_linger(bool on, int seconds) {
        struct linger l;
        l.l_onoff = on ? 1 : 0;
        l.l_linger = seconds;
        int ret = ::setsockopt((int)sockfd_, SOL_SOCKET, SO_LINGER,
                               (const char*)&l, sizeof(l));
        if (ret < 0) {
            throw std::system_error(
#if defined(CORONET_PLATFORM_WINDOWS)
                WSAGetLastError(), std::system_category(), "socket_base::set_linger"
#else
                errno, std::generic_category(), "socket_base::set_linger"
#endif
            );
        }
        return static_cast<Derived&>(*this);
    }

    /// 设置非阻塞模式
    void set_nonblocking() {
#if defined(CORONET_PLATFORM_LINUX)
        int flags = ::fcntl((int)sockfd_, F_GETFL, 0);
        if (flags < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "socket_base::set_nonblocking (F_GETFL)");
        }
        if (::fcntl((int)sockfd_, F_SETFL, flags | O_NONBLOCK) < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "socket_base::set_nonblocking (F_SETFL)");
        }
#elif defined(CORONET_PLATFORM_WINDOWS)
        u_long mode = 1;
        if (::ioctlsocket((SOCKET)sockfd_, FIONBIO, &mode) != 0) {
            throw std::system_error(WSAGetLastError(), std::system_category(),
                                    "socket_base::set_nonblocking");
        }
#endif
    }

    // ---- 地址查询 ----

    /// 获取本地地址
    /// @return 成功返回地址，失败返回 std::nullopt
    [[nodiscard]] std::optional<inet_address> local_addr() const {
        struct sockaddr_storage ss;
        socklen_t len = sizeof(ss);
        if (::getsockname((int)sockfd_, (struct sockaddr*)&ss, &len) < 0) {
            return std::nullopt;
        }
        return inet_address{*reinterpret_cast<const struct sockaddr*>(&ss)};
    }

    /// 获取对端地址
    /// @return 成功返回地址，未连接/失败返回 std::nullopt
    [[nodiscard]] std::optional<inet_address> peer_addr() const {
        struct sockaddr_storage ss;
        socklen_t len = sizeof(ss);
        if (::getpeername((int)sockfd_, (struct sockaddr*)&ss, &len) < 0) {
            return std::nullopt;
        }
        return inet_address{*reinterpret_cast<const struct sockaddr*>(&ss)};
    }

    // ---- 异步操作 ----

    /// 异步关闭：先置 invalid 再 co_await，防止双重关闭
    [[nodiscard("Did you forget to co_await?")]]
    auto close() noexcept {
        auto fd = sockfd_;
        sockfd_ = platform::invalid_socket;
        return async::close((int)fd);
    }

protected:
    /// protected 构造 — 只有派生类（tcp_socket / udp_socket）可以创建
    explicit socket_base(int fd) noexcept
        : sockfd_(static_cast<platform::socket_handle_t>(fd)) {}

    platform::socket_handle_t sockfd_;
};

} // namespace coronet
