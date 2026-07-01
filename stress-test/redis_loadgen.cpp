/// Redis PING benchmark — redis-benchmark compatible CLI & output.
///
/// Mimics: redis-benchmark -h <host> -p <port> -c <clients> -n <requests> \
///                         -P <pipeline> -t <test> -q
///
/// Key features:
///   - Pipeline support (-P N): send N PINGs, recv N PONGs per round-trip
///   - Multi-port (-p 6390-6395 or -p 6390,6391,6392)
///   - Output matches redis-benchmark -q format for easy parsing
///
/// Usage:
///   redis_loadgen -c 50 -n 10000 -p 6379 -t ping -q
///   redis_loadgen -c 60 -n 100000 -p 6390-6395 -P 16

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <string_view>
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

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t INVALID_S = INVALID_SOCKET;
#define CLOSE_S closesocket
#else
using socket_t = int;
constexpr socket_t INVALID_S = (-1);
#define CLOSE_S close
#endif

constexpr int         kDefaultPort   = 6379;
constexpr int         kDefaultClients = 50;
constexpr int64_t     kDefaultReqs   = 100000;
constexpr int         kDefaultPipeline = 1;
constexpr const char* kPingMsg       = "PING\r\n";
constexpr int         kPingLen       = 6;
constexpr int         kPongLen       = 7;   // "+PONG\r\n"

// ============================================================
// Config
// ============================================================
struct Config {
    std::string         host     = "127.0.0.1";
    std::vector<int>    ports;
    int                 clients  = kDefaultClients;
    int64_t             requests = kDefaultReqs;
    int                 pipeline = kDefaultPipeline;
    bool                quiet    = false;
};

// ============================================================
// Port parser — single, list, range, mixed
// ============================================================
static std::vector<int> parse_ports(const char* arg) {
    std::vector<int> result;
    std::string s(arg);
    size_t pos = 0;
    while (pos < s.length()) {
        while (pos < s.length() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
        if (pos >= s.length()) break;
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.length();
        std::string seg = s.substr(pos, end - pos);
        pos = end + 1;
        while (!seg.empty() && (seg.back() == ' ' || seg.back() == '\t'))
            seg.pop_back();
        if (seg.empty()) continue;
        auto dash = seg.find('-');
        if (dash != std::string::npos && dash > 0 && dash < seg.length() - 1) {
            int from = std::atoi(seg.substr(0, dash).c_str());
            int to   = std::atoi(seg.substr(dash + 1).c_str());
            if (from <= 0 || to <= 0 || from > 65535 || to > 65535) continue;
            if (from > to) std::swap(from, to);
            for (int p = from; p <= to; ++p) result.push_back(p);
        } else {
            int p = std::atoi(seg.c_str());
            if (p > 0 && p <= 65535) result.push_back(p);
        }
    }
    return result;
}

// ============================================================
// Parse args — redis-benchmark compatible flags
// ============================================================
static Config parse_args(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if ((arg == "-h" || arg == "--host") && i + 1 < argc)
            cfg.host = argv[++i];
        else if ((arg == "-p" || arg == "--port") && i + 1 < argc)
            cfg.ports = parse_ports(argv[++i]);
        else if ((arg == "-c" || arg == "--clients") && i + 1 < argc)
            cfg.clients = std::atoi(argv[++i]);
        else if ((arg == "-n" || arg == "--requests") && i + 1 < argc)
            cfg.requests = std::atoll(argv[++i]);
        else if ((arg == "-P" || arg == "--pipeline") && i + 1 < argc)
            cfg.pipeline = std::atoi(argv[++i]);
        else if ((arg == "-t" || arg == "--test") && i + 1 < argc)
            ; // only "ping" is meaningful, ignore value
        else if (arg == "-q" || arg == "--quiet")
            cfg.quiet = true;
    }
    if (cfg.ports.empty()) cfg.ports.push_back(kDefaultPort);
    if (cfg.clients < 1) cfg.clients = 1;
    if (cfg.pipeline < 1) cfg.pipeline = 1;
    return cfg;
}

// ============================================================
// TCP connection
// ============================================================
static socket_t create_connection(const char* host, int port) {
    socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_S) return INVALID_S;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, host, &addr.sin_addr);

    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&flag), sizeof(flag));

    if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) < 0) {
        CLOSE_S(sock);
        return INVALID_S;
    }
    return sock;
}

// ============================================================
// Worker thread — pipeline N requests per round-trip
// ============================================================
static void worker_thread(const Config& cfg,
                          std::atomic<int64_t>& total_count,
                          std::atomic<int64_t>& error_count,
                          std::atomic<bool>& stop_flag,
                          int conn_id, int port) {
    socket_t sock = create_connection(cfg.host.c_str(), port);
    if (sock == INVALID_S) {
        std::fprintf(stderr, "[%d] connect failed port %d\n", conn_id, port);
        error_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    int pl = cfg.pipeline;
    // Pre-built send buffer: pl copies of "PING\r\n"
    std::vector<char> send_buf(static_cast<size_t>(pl) * kPingLen);
    for (int i = 0; i < pl; ++i)
        std::memcpy(&send_buf[i * kPingLen], kPingMsg, kPingLen);

    // Receive buffer for pl PONGs
    std::vector<char> recv_buf(static_cast<size_t>(pl) * kPongLen);

    int send_total = pl * kPingLen;
    int recv_total = pl * kPongLen;

    while (!stop_flag.load(std::memory_order_relaxed)) {
        // Send pl PINGs
        int sent = 0;
        while (sent < send_total) {
            int n = ::send(sock, send_buf.data() + sent, send_total - sent, 0);
            if (n <= 0) goto done;
            sent += n;
        }

        // Recv pl PONGs
        int recvd = 0;
        while (recvd < recv_total) {
            int n = ::recv(sock, recv_buf.data() + recvd, recv_total - recvd, 0);
            if (n <= 0) goto done;
            recvd += n;
        }

        total_count.fetch_add(pl, std::memory_order_relaxed);
    }
done:
    CLOSE_S(sock);
}

// ============================================================
// Main
// ============================================================
int main(int argc, char* argv[]) {
    Config cfg = parse_args(argc, argv);

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    int nports = static_cast<int>(cfg.ports.size());

    // --- Header (skip in quiet mode) ---
    if (!cfg.quiet) {
        std::printf("====== PING_INLINE ======\n");
    }

    std::atomic<int64_t> count{0};
    std::atomic<int64_t> errors{0};
    std::atomic<bool>    stop{false};
    std::vector<std::thread> threads;

    auto t_start = std::chrono::steady_clock::now();

    // Round-robin port distribution
    for (int i = 0; i < cfg.clients; ++i) {
        int port = cfg.ports[i % nports];
        threads.emplace_back(worker_thread, std::ref(cfg),
                             std::ref(count), std::ref(errors),
                             std::ref(stop), i, port);
    }

    // Poll until total requests reached
    while (count.load(std::memory_order_relaxed) < cfg.requests) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& t : threads) t.join();

    auto t_end = std::chrono::steady_clock::now();
    double total_sec = std::chrono::duration<double>(t_end - t_start).count();
    int64_t done = count.load();
    double rps = (total_sec > 0.0) ? static_cast<double>(done) / total_sec : 0.0;

    // --- Output (redis-benchmark -q compatible) ---
    if (cfg.quiet) {
        std::printf("PING_INLINE: %.2f requests per second\n", rps);
    } else {
        std::printf("  %lld requests completed in %.2f seconds\n",
                    (long long)done, total_sec);
        std::printf("  %d parallel clients\n", cfg.clients);
        std::printf("  %d bytes payload\n", kPingLen);
        std::printf("  pipeline: %d\n", cfg.pipeline);
        std::printf("  keep alive: 1\n");
        if (errors.load() > 0)
            std::printf("  errors: %lld\n", (long long)errors.load());
        std::printf("\n%.2f requests per second\n", rps);
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
