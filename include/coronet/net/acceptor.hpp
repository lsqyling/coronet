#pragma once

#include "coronet/net/socket.hpp"

namespace coronet {

/// TCP listener that yields connected sockets via async::accept.
class acceptor {
public:
    explicit acceptor(const inet_address& listen_addr);

    ~acceptor() = default;
    acceptor(acceptor&&) = default;
    acceptor& operator=(acceptor&&) = default;

    acceptor(const acceptor&) = delete;
    acceptor& operator=(const acceptor&) = delete;

    [[nodiscard("Did you forget to co_await?")]]
    auto accept(int flags = 0) noexcept {
        return async::accept((int)listen_socket_.native_handle(),
                             nullptr, nullptr, flags);
    }

    [[nodiscard]] platform::socket_handle_t listen_fd() const noexcept {
        return listen_socket_.native_handle();
    }

private:
    socket listen_socket_;
};

inline acceptor::acceptor(const inet_address& listen_addr)
    : listen_socket_(socket::create_tcp(listen_addr.family())) {
    listen_socket_.set_reuse_addr(true)
                   .bind(listen_addr)
                   .listen();
}

} // namespace coronet
