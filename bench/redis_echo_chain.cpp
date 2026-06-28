/// coronet coroutine-based Redis echo server with chained co_await.
/// Uses: co_await (sock.send(pong) && sock.recv(buf))
/// Usage: ./redis_echo_chain [port]

#include <coronet/coronet.hpp>
#include <coronet/io_context.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

constexpr int DefaultPort = 6379;
constexpr int BufSize = 4096;

coronet::task<> redis_session(int sockfd) {
    coronet::socket sock{sockfd};
    char buf[BufSize];
    constexpr const char* pong = "+PONG\r\n";
    constexpr int pong_len = 7;

    int n = co_await sock.recv(buf);
    while (n > 0) {
        n = co_await (sock.send({pong, static_cast<size_t>(pong_len)}) && sock.recv(buf));
    }
}

coronet::task<> redis_server(uint16_t port) {
    coronet::acceptor ac{coronet::inet_address{port}};
    std::fprintf(stderr, "[coronet chain] listening on port %d\n", port);
    std::fflush(stderr);

    while (true) {
        int sock = co_await ac.accept();
        if (sock >= 0) {
            coronet::co_spawn(redis_session(sock));
        }
    }
}

int main(int argc, char* argv[]) {
    uint16_t port = DefaultPort;
    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));

    std::fprintf(stderr, "[coronet chain] Starting (chained co_await)\n");
    std::fprintf(stderr, "[coronet chain] Port: %d\n", port);
    std::fflush(stderr);

    coronet::io_context ctx;
    ctx.co_spawn(redis_server(port));
    ctx.start();
    ctx.join();

    return 0;
}
