/// coronet multi-threaded Redis echo server (multi-port approach).
/// Each worker has its own io_context + port. Run all in parallel.
/// Usage: ./redis_echo_MT_reuseport [base_port] [threads]
///
/// For comparison with co_context's cross-thread co_spawn pattern,
/// we test throughput by running load across all worker ports simultaneously.

#include <coronet/coronet.hpp>
#include <coronet/io_context.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

constexpr int DefaultBasePort = 6380;
constexpr int DefaultThreads = 6;
constexpr int BufSize = 4096;

coronet::task<> redis_session(int sockfd) {
    coronet::socket sock{sockfd};
    char buf[BufSize];
    constexpr const char* pong = "+PONG\r\n";
    constexpr int pong_len = 7;

    while (true) {
        int nr = co_await sock.recv(buf);
        if (nr <= 0) break;
        int ns = co_await sock.send({pong, static_cast<size_t>(pong_len)});
        if (ns <= 0) break;
    }
}

coronet::task<> redis_worker(uint16_t port, int worker_id) {
    coronet::acceptor ac{coronet::inet_address{port}};
    std::fprintf(stderr, "[worker %d] port=%d\n", worker_id, port);
    std::fflush(stderr);

    while (true) {
        int sock = co_await ac.accept();
        if (sock >= 0) {
            coronet::co_spawn(redis_session(sock));
        }
    }
}

int main(int argc, char* argv[]) {
    int base_port = DefaultBasePort;
    if (argc > 1) base_port = std::atoi(argv[1]);
    int nthreads = DefaultThreads;
    if (argc > 2) nthreads = std::atoi(argv[2]);
    if (nthreads < 1) nthreads = 1;

    std::fprintf(stderr, "[coronet MT] %d workers, ports %d-%d\n",
                 nthreads, base_port, base_port + nthreads - 1);
    std::fflush(stderr);

    std::vector<coronet::io_context> workers(nthreads);
    std::vector<std::thread> threads;

    for (int i = 0; i < nthreads; ++i) {
        auto port = static_cast<uint16_t>(base_port + i);
        workers[i].co_spawn(redis_worker(port, i));
        threads.emplace_back([&workers, i] {
            workers[i].start();
        });
    }

    for (auto& t : threads) t.join();
    return 0;
}
