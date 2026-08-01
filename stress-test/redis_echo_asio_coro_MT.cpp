/// ASIO coroutine multi-threaded Redis echo server - shared io_context model.
/// Mirrors redis_echo_MT.cpp (coronet) architecture:
///   - 1 acceptor coroutine co_spawn'd once
///   - N threads call io.run() on the same io_context (ASIO thread-safety)
///   - Sessions co_spawn'd from the accept loop are distributed across
///     threads by the io_context - comparable to coronet MT's cross-thread
///     co_spawn round-robin distribution.
/// Usage: ./redis_echo_asio_coro_MT [port] [threads]

#include <asio.hpp>

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <vector>

using asio::ip::tcp;

constexpr int DefaultPort    = 6379;
constexpr int DefaultThreads = 6;
constexpr int BufSize        = 4096;

/// Per-session coroutine - recv / send PONG in a loop
asio::awaitable<void> redis_session(tcp::socket sock) {
    char buf[BufSize];
    constexpr std::string_view pong = "+PONG\r\n";

    try {
        while (true) {
            std::size_t nr = co_await sock.async_read_some(asio::buffer(buf), asio::use_awaitable);
            if (nr == 0) break;  // peer closed
            co_await asio::async_write(sock, asio::buffer(pong.data(), pong.size()),
                                       asio::use_awaitable);
        }
    } catch (const asio::system_error&) {
        // peer reset / operation aborted - session over
    }
}

/// Accept loop - co_spawns one detached session coroutine per connection
asio::awaitable<void> redis_server(asio::io_context& io, uint16_t port) {
    tcp::acceptor ac(io, tcp::endpoint(tcp::v4(), port));
    std::fprintf(stderr, "[ASIO coro MT] listening on port %d (coroutine-based)\n", port);
    std::fflush(stderr);

    while (true) {
        try {
            tcp::socket sock = co_await ac.async_accept(asio::use_awaitable);
            asio::co_spawn(io, redis_session(std::move(sock)), asio::detached);
        } catch (const asio::system_error& e) {
            if (e.code() == asio::error::operation_aborted) break;
            // transient accept error - keep accepting
        }
    }
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
#endif
    uint16_t port = DefaultPort;
    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));

    int nthreads = DefaultThreads;
    if (argc > 2) nthreads = std::atoi(argv[2]);
    if (nthreads < 1) nthreads = 1;

    std::fprintf(stderr, "[ASIO coro MT] %d threads, port %d (coroutine-based)\n",
                 nthreads, port);
    std::fflush(stderr);

    asio::io_context io;
    asio::co_spawn(io, redis_server(io, port), asio::detached);

    std::vector<std::thread> threads;
    for (int i = 0; i < nthreads; ++i) {
        threads.emplace_back([&io] { io.run(); });
    }

    for (auto& t : threads) t.join();
    return 0;
}
