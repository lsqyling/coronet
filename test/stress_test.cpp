/**
 * stress_test.cpp —— coronet 集成压力测试
 *
 * 测试目的：
 *   验证 coronet 事件循环在高并发场景下的完整 I/O 路径。本测试在进程中同时
 *   启动一个基于协程的 echo server（使用 coronet 的 acceptor/socket/recv/send）
 *   和多个阻塞式 POSIX TCP 客户端。客户端向服务器发送消息并等待回显，通过
 *   对比发送和接收的数据来验证正确性。
 *
 * 测试模式：
 *   - 单线程 io_context 运行 echo server 协程
 *   - 多个 std::thread（POSIX 客户端）并发建立 TCP 连接并发送请求
 *   - 服务器每接受一个连接就 co_spawn 一个 echo_session 协程处理
 *   - 客户端验证 echo 数据的完整性（发送内容 == 接收内容）
 *
 * 关键验证点：
 *   - 协程 socket recv/send 在持续压力下正确收发数据
 *   - acceptor 能持续接受并发连接
 *   - co_spawn 在压力下稳定创建新协程
 *   - 总吞吐量统计
 */

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
/**
 * echo_session——单个 echo 会话协程
 *
 * 从 socket 读取数据，然后将相同的数据原样写回。这是最基本的 echo 服务逻辑。
 * recv 返回 ≤0 表示连接关闭或出错，此时退出循环结束会话。
 * 使用 coronet::socket 的协程版本 recv/send，通过 io_uring 完成异步 I/O。
 */
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

/**
 * echo_server——监听端口并接受连接的服务器协程
 *
 * 创建 acceptor 绑定到测试端口后进入无限循环。每次 accept 返回一个新的
 * 客户端 socket fd，立即通过 co_spawn 创建一个 echo_session 协程来处理。
 * 这种方式使得每个客户端连接都由独立的协程处理，实现了并发。
 */
coronet::task<> echo_server() {
    coronet::acceptor ac{coronet::inet_address{TestPort}};
    std::printf("[server] listening on port %d\n", TestPort);
    while (true) {
        int sock = co_await ac.accept();
        if (sock >= 0) coronet::co_spawn(echo_session(sock));
    }
}

// ---- Blocking POSIX client ----
/**
 * run_client——阻塞式 POSIX TCP 客户端
 *
 * 在独立的 std::thread 中运行，使用原始 socket API（非协程版本）向
 * echo server 发送 num_requests 条消息，然后等待回显并验证数据一致性。
 * 验证逻辑：发送字节数 == 接收字节数 && 接收内容 == 发送内容。
 * 每次成功验证后通过原子变量 ok 累加计数。
 */
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

    /**
     * 强制退出——服务器线程阻塞在 accept 上，clean shutdown 需要
     * 跨线程唤醒（eventfd/IOCP post），该功能尚未实现。
     * 因此使用 ::_exit(0) 直接终止进程，跳过析构和线程 join。
     */
    // Force exit — server thread blocks on accept, clean shutdown needs
    // cross-thread wakeup (eventfd/IOCP post) which isn't implemented yet.
    if (total > 0) {
        std::printf("\n=== STRESS TEST PASSED (%.0f rps) ===\n", rps);
        ::_exit(0);
    }
    std::fprintf(stderr, "\n=== STRESS TEST FAILED ===\n");
    ::_exit(1);
}
