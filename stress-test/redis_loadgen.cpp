/// Custom Redis PING load generator.
/// Opens N concurrent TCP connections, sends PING in a loop,
/// and reports requests per second.
/// Usage: ./redis_loadgen -c <concurrency> -n <total_requests> [-p <port>]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

constexpr int DefaultPort = 6379;
constexpr const char* PingMsg = "PING\r\n";
constexpr int PingLen = 6;
constexpr const char* PongExpect = "+PONG\r\n";

#ifdef _WIN32
using socket_t = SOCKET;
#define INVALID_S INVALID_SOCKET
#define CLOSE_S closesocket
#else
using socket_t = int;
#define INVALID_S (-1)
#define CLOSE_S close
#endif

struct Config {
    int concurrency = 10;
    int total_requests = 100000;
    int port = DefaultPort;
    const char* host = "127.0.0.1";
};

Config parse_args(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            cfg.concurrency = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            cfg.total_requests = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            cfg.port = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "-h") == 0 && i + 1 < argc)
            cfg.host = argv[++i];
    }
    return cfg;
}

socket_t create_connection(const char* host, int port) {
    socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_S) return INVALID_S;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, host, &addr.sin_addr);

    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));

    if (::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        CLOSE_S(sock);
        return INVALID_S;
    }
    return sock;
}

void worker_thread(const Config& cfg, std::atomic<int64_t>& total_count,
                   std::atomic<bool>& stop_flag, int conn_id) {
    socket_t sock = create_connection(cfg.host, cfg.port);
    if (sock == INVALID_S) {
        std::fprintf(stderr, "Thread %d: failed to connect\n", conn_id);
        return;
    }

    char buf[128];
    while (!stop_flag.load(std::memory_order_relaxed)) {
        // Send PING
        int ns = ::send(sock, PingMsg, PingLen, 0);
        if (ns <= 0) break;

        // Receive PONG
        int nr = ::recv(sock, buf, sizeof(buf), 0);
        if (nr <= 0) break;

        total_count.fetch_add(1, std::memory_order_relaxed);
    }
    CLOSE_S(sock);
}

int main(int argc, char* argv[]) {
    Config cfg = parse_args(argc, argv);

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    std::printf("Redis Load Generator\n");
    std::printf("  Target: %s:%d\n", cfg.host, cfg.port);
    std::printf("  Concurrency: %d\n", cfg.concurrency);
    std::printf("  Total requests: %d\n\n", cfg.total_requests);

    std::atomic<int64_t> count{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;

    auto start_time = std::chrono::steady_clock::now();

    // Start worker threads
    for (int i = 0; i < cfg.concurrency; i++) {
        threads.emplace_back(worker_thread, std::ref(cfg), std::ref(count),
                             std::ref(stop), i);
    }

    // Monitor progress and stop when enough requests completed
    while (count.load(std::memory_order_relaxed) < cfg.total_requests) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        int64_t current = count.load(std::memory_order_relaxed);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        if (elapsed > 0) {
            double rps = current * 1000.0 / elapsed;
            std::printf("\r  Progress: %lld/%d requests, %.2f req/s",
                        (long long)current, cfg.total_requests, rps);
            std::fflush(stdout);
        }
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& t : threads) t.join();

    auto end_time = std::chrono::steady_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();

    double rps = count.load() * 1000.0 / total_ms;
    std::printf("\n\n=== Results ===\n");
    std::printf("  Total: %lld requests\n", (long long)count.load());
    std::printf("  Time:  %lld ms\n", (long long)total_ms);
    std::printf("  RPS:   %.2f\n", rps);

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
