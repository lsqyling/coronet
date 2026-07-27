#pragma once

// ============================================================
// tcp_acceptor.hpp — TCP 连接接收器
// ============================================================
// 绑定地址并监听，通过 accept 生成已连接的 tcp_socket。
//
// accept 方法选择：
//   accept()          — 返回裸 fd (int)，向后兼容，存在 RAII 缺口
//   accept_socket()   — 返回 task<tcp_socket>，RAII 安全（推荐）
//   accept_with_peer() — 返回 task<accept_result>，RAII 安全 + 对端地址
//
// 向后兼容：using acceptor = tcp_acceptor;
//
// 满足 listener concept。

#include "coronet/net/tcp_socket.hpp"
#include "coronet/task.hpp"

namespace coronet {

/// accept 返回的连接结果（包含 socket 和对端地址）。
struct accept_result {
    tcp_socket conn;
    inet_address peer;
};

/// TCP 连接接收器 — 绑定地址、监听、accept 生成已连接 socket。
///
/// 满足 listener concept。
///
/// 使用示例：
///   tcp_acceptor acceptor{inet_address{8080}};
///   while (true) {
///       auto conn = co_await acceptor.accept_socket();
///       co_spawn(session(std::move(conn)));
///   }
class tcp_acceptor {
public:
    /// 构造：创建 TCP 套接字 → set_reuse_addr → bind → listen
    ///
    /// @param listen_addr 监听地址
    /// @param backlog 监听队列长度（默认 SOMAXCONN）
    explicit tcp_acceptor(const inet_address& listen_addr, int backlog = SOMAXCONN);

    ~tcp_acceptor() = default;
    tcp_acceptor(tcp_acceptor&&) = default;
    tcp_acceptor& operator=(tcp_acceptor&&) = default;

    tcp_acceptor(const tcp_acceptor&) = delete;
    tcp_acceptor& operator=(const tcp_acceptor&) = delete;

    /// 异步接受连接（返回裸 fd）。
    /// @note 存在 RAII 缺口，推荐使用 accept_socket()。
    [[nodiscard("Did you forget to co_await?")]]
    auto accept(int flags = 0) noexcept {
        return async::accept((int)listen_socket_.native_handle(),
                             nullptr, nullptr, flags);
    }

    /// 异步接受连接（返回 RAII tcp_socket）。
    [[nodiscard("Did you forget to co_await?")]]
    task<tcp_socket> accept_socket(int flags = 0) {
        int fd = co_await async::accept(
            (int)listen_socket_.native_handle(),
            nullptr, nullptr, flags);
        co_return tcp_socket{fd};
    }

    /// 异步接受连接（返回 tcp_socket + 对端地址）。
    ///
    /// 使用 getpeername 获取对端地址，跨平台可靠。
    /// IOCP 的 AcceptEx 需要 GetAcceptExSockaddrs 解析，实现复杂，
    /// getpeername 在 SO_UPDATE_ACCEPT_CONTEXT 后可靠工作，
    /// 性能开销可忽略（一次系统调用，无网络 I/O）。
    [[nodiscard("Did you forget to co_await?")]]
    task<accept_result> accept_with_peer(int flags = 0) {
        int fd = co_await async::accept(
            (int)listen_socket_.native_handle(),
            nullptr, nullptr, flags);
        tcp_socket conn{fd};
        auto peer = conn.peer_addr();
        co_return accept_result{
            std::move(conn),
            peer.value_or(inet_address{})
        };
    }

    /// 获取监听套接字的原生句柄
    [[nodiscard]] platform::socket_handle_t listen_fd() const noexcept {
        return listen_socket_.native_handle();
    }

private:
    tcp_socket listen_socket_;
};

/// 向后兼容别名 — 旧代码中的 acceptor 映射到 tcp_acceptor
using acceptor = tcp_acceptor;

// ---- 内联实现 ----

inline tcp_acceptor::tcp_acceptor(const inet_address& listen_addr, int backlog)
    : listen_socket_(tcp_socket::create_tcp(listen_addr.family())) {
    listen_socket_.set_reuse_addr(true);
    listen_socket_.bind(listen_addr);
    listen_socket_.listen(backlog);
}

} // namespace coronet
