/// ASIO multi-threaded Redis echo server.
/// Multiple threads share one io_context (ASIO handles thread safety internally).
/// Usage: ./redis_echo_asio_MT [port] [threads]

#include <asio.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

using asio::ip::tcp;

constexpr int DefaultPort = 6379;
constexpr int DefaultThreads = 6;
constexpr int BufSize = 4096;

class redis_session : public std::enable_shared_from_this<redis_session> {
public:
    explicit redis_session(tcp::socket socket) : socket_(std::move(socket)) {}
    void start() { do_read(); }

private:
    void do_read() {
        auto self(shared_from_this());
        socket_.async_read_some(asio::buffer(data_, BufSize),
            [this, self](std::error_code ec, std::size_t /*length*/) {
                if (ec) return;
                constexpr const char* pong = "+PONG\r\n";
                constexpr std::size_t pong_len = 7;
                do_write(pong, pong_len);
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
        : acceptor_(io, tcp::endpoint(tcp::v4(), port))
    {
        std::fprintf(stderr, "[ASIO MT] listening on port %d\n", port);
        std::fflush(stderr);
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

    tcp::acceptor acceptor_;
};

int main(int argc, char* argv[]) {
    uint16_t port = DefaultPort;
    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));

    int nthreads = DefaultThreads;
    if (argc > 2) nthreads = std::atoi(argv[2]);
    if (nthreads < 1) nthreads = 1;

    std::fprintf(stderr, "[ASIO MT] %d threads, port %d\n", nthreads, port);
    std::fflush(stderr);

    asio::io_context io;
    redis_server server(io, port);

    std::vector<std::thread> threads;
    for (int i = 0; i < nthreads; ++i) {
        threads.emplace_back([&io] { io.run(); });
    }

    for (auto& t : threads) t.join();
    return 0;
}
