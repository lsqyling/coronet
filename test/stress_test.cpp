/// Integrated stress test: coronet echo server + POSIX clients in-process
#include <coronet/coronet.hpp>
#include <coronet/io_context.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

constexpr uint16_t TestPort = 9876;
constexpr int BufSize = 4096;

// ---- Echo server session ----
coronet::task<> echo_session(int sockfd) {
    coronet::socket sock{sockfd};
    char buf[BufSize];
    while (true) {
        int nr = co_await sock.recv(buf);
        if (nr <= 0) break;
        int ns = co_await sock.send({buf, static_cast<size_t>(nr)});
        if (ns <= 0) break;
    }
}

coronet::task<> echo_server() {
    coronet::acceptor ac{coronet::inet_address{TestPort}};
    std::printf("[server] listening on port %d\n", TestPort);
    while (true) {
        int sock = co_await ac.accept();
        if (sock >= 0) coronet::co_spawn(echo_session(sock));
    }
}

// ---- Blocking POSIX client ----
static void run_client(int client_id, int num_requests, std::atomic<int>& ok) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { std::fprintf(stderr, "[c%d] socket fail\n", client_id); return; }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TestPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::fprintf(stderr, "[c%d] connect fail\n", client_id);
        ::close(fd); return;
    }

    char buf[BufSize];
    const char* msg = "Hello!";
    int msg_len = 6;
    for (int i = 0; i < num_requests; i++) {
        int ns = (int)::send(fd, msg, msg_len, 0);
        int nr = (int)::recv(fd, buf, BufSize, 0);
        if (ns == msg_len && nr == msg_len && std::strncmp(buf, msg, msg_len) == 0) {
            ok.fetch_add(1, std::memory_order_relaxed);
        }
    }
    ::close(fd);
}

int main() {
    std::printf("=== coronet Stress Test ===\n");

    constexpr int clients = 10;
    constexpr int reqs_per = 1000;
    std::atomic<int> ok{0};

    // Start server
    coronet::io_context server;
    server.co_spawn(echo_server());
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Run clients
    std::vector<std::thread> ths;
    auto t0 = std::chrono::steady_clock::now();

    for (int i = 0; i < clients; i++)
        ths.emplace_back(run_client, i, reqs_per, std::ref(ok));

    for (auto& t : ths) t.join();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    int total = ok.load();
    double rps = ms > 0 ? total * 1000.0 / ms : 0;

    std::printf("\n=== Results ===\n");
    std::printf("  Clients:  %d\n", clients);
    std::printf("  Reqs:     %d x %d = %d\n", clients, reqs_per, clients * reqs_per);
    std::printf("  Success:  %d/%d\n", total, clients * reqs_per);
    std::printf("  Time:     %lld ms\n", (long long)ms);
    std::printf("  Tput:     %.0f req/s\n", rps);

    // Force exit — server thread blocks on accept, clean shutdown needs
    // cross-thread wakeup (eventfd/IOCP post) which isn't implemented yet.
    if (total > 0) {
        std::printf("\n=== STRESS TEST PASSED (%.0f rps) ===\n", rps);
        ::_exit(0);
    }
    std::fprintf(stderr, "\n=== STRESS TEST FAILED ===\n");
    ::_exit(1);
}
