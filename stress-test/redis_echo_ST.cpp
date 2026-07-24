/// coronet coroutine-based Redis echo server.
/// Responds to PING with +PONG\r\n (RESP protocol).
/// Usage: ./redis_echo_coro [port]

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

    while (true) {
        int nr = co_await sock.recv(buf);
        if (nr <= 0) break;
        // Respond +PONG\r\n to any request (redis-benchmark sends CONFIG GET, PING, etc.)
        int ns = co_await sock.send({pong, static_cast<size_t>(pong_len)});
        if (ns <= 0) break;
    }
}

coronet::task<> redis_server(uint16_t port) {
    coronet::acceptor ac{coronet::inet_address{port}};
    std::fprintf(stderr, "[coronet ST] listening on port %d\n", port);
    std::fflush(stderr);

    while (true) {
        int sock = co_await ac.accept();
        if (sock >= 0) {
            co_spawn(redis_session(sock));
        }
    }
}

int main(int argc, char* argv[]) {
    // 抑制 Debug 模式下的 abort() 阻塞对话框，让进程直接退出
    // 避免 CTest 环境下弹窗阻塞 → 端口泄漏 → 下次测试冲突的死循环
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
#endif
    uint16_t port = DefaultPort;
    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));

    // Use stderr for unbuffered startup messages
    std::fprintf(stderr, "[coronet ST] Starting Redis echo server (coroutine-based)\n");
    std::fprintf(stderr, "[coronet ST] Port: %d\n", port);
    std::fflush(stderr);

    coronet::io_context ctx;
    ctx.co_spawn(redis_server(port));
    ctx.start();
    ctx.join();

    return 0;
}
