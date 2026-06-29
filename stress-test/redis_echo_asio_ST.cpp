/// ASIO callback-based Redis echo server.
/// Responds to PING with +PONG\\r\\n (RESP protocol).
/// Usage: ./redis_echo_asio [port]

#include <asio.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

using asio::ip::tcp;

constexpr int DefaultPort = 6379;
constexpr int BufSize = 4096;

class redis_session : public std::enable_shared_from_this<redis_session> {
public:
    redis_session(tcp::socket socket) : socket_(std::move(socket)) {}

    void start() { do_read(); }

private:
    void do_read() {
        auto self(shared_from_this());
        socket_.async_read_some(asio::buffer(data_, BufSize),
            [this, self](std::error_code ec, std::size_t length) {
                if (ec) return;
                // Respond +PONG\r\n to any request
                constexpr const char* pong = "+PONG\r\n";
                do_write(pong, 7);
            });
    }

    void do_write(const char* data, size_t len) {
        auto self(shared_from_this());
        asio::async_write(socket_, asio::buffer(data, len),
            [this, self](std::error_code ec, std::size_t /*len*/) {
                if (!ec) do_read();
            });
    }

    tcp::socket socket_;
    char data_[BufSize];
};

class redis_server {
public:
    redis_server(asio::io_context& io, uint16_t port)
        : io_(io), acceptor_(io, tcp::endpoint(tcp::v4(), port))
    {
        std::printf("[ASIO] Redis echo server listening on port %d\n", port);
        do_accept();
    }

private:
    void do_accept() {
        acceptor_.async_accept(
            [this](std::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<redis_session>(std::move(socket))->start();
                }
                do_accept();
            });
    }

    asio::io_context& io_;
    tcp::acceptor acceptor_;
};

int main(int argc, char* argv[]) {
    uint16_t port = DefaultPort;
    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));

    std::fprintf(stderr, "[ASIO ST] Starting Redis echo server (callback-based)\n");
    std::fprintf(stderr, "[ASIO ST] Port: %d\n", port);
    std::fflush(stderr);

    asio::io_context io;
    redis_server server(io, port);
    io.run();

    return 0;
}
