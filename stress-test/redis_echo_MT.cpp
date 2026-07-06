/// coronet multi-threaded Redis echo server — shared-port model.
/// Uses cross-thread co_spawn to distribute accepted connections
/// across N io_context workers, all sharing a single listen port.
///
/// Architecture (comparable to ASIO_MT):
///   - 1 acceptor coroutine on worker[0] → single port
///   - N worker io_contexts (1 thread each) → N event loops
///   - Accepted sessions distributed round-robin via cross-thread co_spawn
///
/// Usage: ./redis_echo_MT [port] [threads]

#include <coronet/coronet.hpp>
#include <coronet/io_context.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

constexpr int DefaultPort    = 6379;
constexpr int DefaultThreads = 6;
constexpr int BufSize        = 4096;

/// Per-session coroutine — recv / send PONG in a loop
coronet::task<> redis_session(int sockfd) {
    coronet::socket sock{sockfd};
    char buf[BufSize];
    constexpr const char* pong     = "+PONG\r\n";
    constexpr int         pong_len = 7;

    while (true) {
        int nr = co_await sock.recv(buf);
        if (nr <= 0) break;
        int ns = co_await sock.send({pong, static_cast<size_t>(pong_len)});
        if (ns <= 0) break;
    }
}

/// Acceptor coroutine — runs on worker[0], distributes sessions to all workers
coronet::task<> acceptor_task(uint16_t port,
                              std::vector<coronet::io_context>& workers,
                              int nworkers) {
    coronet::acceptor ac{coronet::inet_address{port}};
    std::fprintf(stderr, "[coronet MT] listening on port %d (%d workers)\n",
                 port, nworkers);
    std::fflush(stderr);

    int next = 0;
    while (true) {
        int sock = co_await ac.accept();
        if (sock >= 0) {
            // Round-robin distribute to workers via cross-thread co_spawn
            workers[next].co_spawn(redis_session(sock));
            next = (next + 1) % nworkers;
        }
    }
}

int main(int argc, char* argv[]) {
    uint16_t port = DefaultPort;
    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));

    int nthreads = DefaultThreads;
    if (argc > 2) nthreads = std::atoi(argv[2]);
    if (nthreads < 1) nthreads = 1;

    std::fprintf(stderr, "[coronet MT] %d workers, shared port %d\n",
                 nthreads, port);
    std::fflush(stderr);

    // Create N io_contexts — one per worker thread
    std::vector<coronet::io_context> workers(nthreads);
    coronet::io_context balancer;

    balancer.co_spawn(acceptor_task(port, workers, nthreads));
    balancer.start();

    for(auto &ctx : workers) {
        ctx.start();
    }
    balancer.join();
    return 0;
}
