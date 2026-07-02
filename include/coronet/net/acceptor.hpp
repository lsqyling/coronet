#pragma once

#include "coronet/net/socket.hpp"

namespace coronet {

/// TCP 连接接收器 —— 绑定地址并监听，通过 accept 生成已连接套接字。
///
/// TCP listener that yields connected sockets via async::accept.
///
/// ## 设计
///
/// acceptor 是 socket 的封装，提供面向连接的 accept 语义：
///
/// 1. 构造时即绑定地址并开始监听（builder 模式链式调用）
/// 2. accept() 返回 awaitable，由 io_context 驱动异步接受连接
/// 3. 不支持拷贝，支持移动语义
///
/// ## 与 socket 的关系
///
/// acceptor 内部持有 listen_socket_（RAII 管理），
/// 但 accept 生成的连接套接字由调用者自行管理（作为 async::accept 的返回值）。
///
/// ## 使用示例
///
/// ```cpp
/// coronet::acceptor acceptor{inet_address{8080}};
/// while (true) {
///     auto conn = co_await acceptor.accept();
///     // 处理连接...
/// }
/// ```
class acceptor {
public:
    /// 构造函数：创建 TCP 套接字、设置地址重用、绑定、监听。
    ///
    /// Constructor: create TCP socket, set reuse, bind, listen.
    /// @param listen_addr 监听地址（含端口）
    explicit acceptor(const inet_address& listen_addr);

    ~acceptor() = default;
    acceptor(acceptor&&) = default;
    acceptor& operator=(acceptor&&) = default;

    acceptor(const acceptor&) = delete;
    acceptor& operator=(const acceptor&) = delete;

    /// 异步接受一个连接。
    ///
    /// Async accept a connection.
    /// @param flags 传递给 async::accept 的标志位
    /// @return 返回新连接的 awaitable（通常 auto 推导为 async::accept 的返回类型）
    [[nodiscard("Did you forget to co_await?")]]
    auto accept(int flags = 0) noexcept {
        return async::accept((int)listen_socket_.native_handle(),
                             nullptr, nullptr, flags);
    }

    /// 获取监听套接字的原生句柄。
    ///
    /// Get the native handle of the listening socket.
    [[nodiscard]] platform::socket_handle_t listen_fd() const noexcept {
        return listen_socket_.native_handle();
    }

private:
    socket listen_socket_;  // RAII 管理的监听套接字
};

/// 构造函数内联实现。
///
/// 构造流程：
///   1. create_tcp(family) — 创建 TCP 套接字（非阻塞 + close-on-exec）
///   2. set_reuse_addr(true) — 允许地址重用（防止 TIME_WAIT 问题）
///   3. bind(listen_addr) — 绑定到指定地址和端口
///   4. listen() — 开始监听
///
/// 这种链式调用（fluent interface）使得构造表达式简洁明了。
inline acceptor::acceptor(const inet_address& listen_addr)
    : listen_socket_(socket::create_tcp(listen_addr.family())) {
    listen_socket_.set_reuse_addr(true)
                   .bind(listen_addr)
                   .listen();
}

} // namespace coronet
