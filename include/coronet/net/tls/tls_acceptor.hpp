#pragma once

// ============================================================
// tls_acceptor.hpp — TLS 连接接收器
// ============================================================
// 包装 tcp_acceptor，在 TCP accept 后自动执行服务端 TLS 握手。
// 满足 listener concept。
//
// 工作流程：
//   1. tcp_acceptor 接受 TCP 连接
//   2. 用 tls_context 创建 tls_socket
//   3. 执行服务端 TLS 握手
//   4. 返回已完成握手的 tls_socket
//
// 使用示例：
//   tls_context ctx{tls_context::mode::server};
//   ctx.load_cert_file("server.crt", "server.key");
//
//   tls_acceptor ac{inet_address{443}, ctx};
//   while (true) {
//       auto conn = co_await ac.accept_socket();
//       co_spawn(tls_session(std::move(conn)));
//   }
//
// 前置条件：CORONET_HAS_TLS 定义时编译，需链接 OpenSSL。

#include "coronet/net/tcp_acceptor.hpp"
#include "coronet/net/tls/tls_context.hpp"
#include "coronet/net/tls/tls_socket.hpp"
#include "coronet/task.hpp"

#ifdef CORONET_HAS_TLS

#include <stdexcept>
#include <string>
#include <utility>

namespace coronet {

/// TLS 连接接收器 — 接受 TCP 连接并执行 TLS 握手。
///
/// 满足 listener concept。
///
/// 内部持有 tcp_acceptor 和 tls_context 的指针。
/// tls_context 必须在 tls_acceptor 存活期间保持有效。
class tls_acceptor {
public:
    /// 构造 TLS 接收器。
    ///
    /// @param listen_addr 监听地址
    /// @param ctx TLS 上下文（服务端模式，含证书和密钥）
    /// @param backlog 监听队列长度（默认 SOMAXCONN）
    explicit tls_acceptor(const inet_address& listen_addr,
                          const tls_context& ctx,
                          int backlog = SOMAXCONN)
        : acceptor_(listen_addr, backlog)
        , ctx_(&ctx) {
    }

    ~tls_acceptor() = default;
    tls_acceptor(tls_acceptor&&) = default;
    tls_acceptor& operator=(tls_acceptor&&) = default;
    tls_acceptor(const tls_acceptor&) = delete;
    tls_acceptor& operator=(const tls_acceptor&) = delete;

    /// 异步接受 TLS 连接。
    ///
    /// 流程：TCP accept → 创建 tls_socket → TLS 握手 → 返回
    ///
    /// @returns 已完成 TLS 握手的 tls_socket
    /// @throws std::runtime_error TLS 握手失败
    [[nodiscard("Did you forget to co_await?")]]
    task<tls_socket> accept_socket() {
        // 1. TCP accept
        tcp_socket tcp_conn = co_await acceptor_.accept_socket();

        // 2. 创建 TLS 套接字
        tls_socket tls_conn{std::move(tcp_conn), *ctx_};

        // 3. 服务端 TLS 握手
        co_await tls_conn.handshake();

        co_return std::move(tls_conn);
    }

    /// 获取底层监听套接字的原生句柄
    [[nodiscard]] platform::socket_handle_t listen_fd() const noexcept {
        return acceptor_.listen_fd();
    }

private:
    tcp_acceptor acceptor_;
    const tls_context* ctx_;
};

} // namespace coronet

#endif // CORONET_HAS_TLS
