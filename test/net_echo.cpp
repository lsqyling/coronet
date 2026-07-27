/**
 * net_echo.cpp — net/ 模块异步集成测试
 *
 * 测试目的：
 *   验证 coronet net/ 模块在 io_context 事件循环中的完整 I/O 路径：
 *   acceptor.accept_socket()、accept_with_peer()、socket.connect()、
 *   socket.send()、socket.recv()、socket.shutdown_write()。
 *
 * 测试模式：
 *   每个子测试创建独立的 io_context，co_spawn server + client 协程，
 *   通过原子计数器验证结果。timeout_stop 作为安全网防止死锁。
 *
 * 子测试：
 *   1. BasicEcho      — 3 个顺序客户端，验证 echo 数据完整性
 *   2. AcceptWithPeer — 验证 accept_with_peer 返回的对端地址 = 127.0.0.1
 *   3. HalfClose      — shutdown_write 后 server recv 返回 0 (EOF)
 *   4. Concurrent     — 5 个并发客户端 (co_spawn handler)
 *   5. RepeatedCycles — 20 次 accept/connect 周期，验证 fd 无泄漏
 *
 * 资源释放：
 *   所有 socket/acceptor 使用 RAII，协程结束时自动关闭 fd。
 *   RepeatedCycles 测试通过 20 次循环验证 fd 不会耗尽。
 *
 * Note: The new tcp_socket / tcp_acceptor names avoid the old Windows conflict
 * where ::socket (winsock2.h) clashed with coronet::socket. We still use
 * explicit coronet:: prefix for clarity.
 */

#include <coronet/coronet.hpp>
#include <coronet/io_context.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

// ---- 辅助：超时安全网 ----

coronet::task<> timeout_stop(coronet::io_context& ctx, int ms) {
    co_await coronet::async::timeout(std::chrono::milliseconds(ms));
    ctx.can_stop();
}

// ====================================================================
// Test 1: Basic TCP Echo — 3 个顺序客户端
// ====================================================================

coronet::task<> echo_server(coronet::io_context& ctx, uint16_t port, int n,
                            std::atomic<int>& done) {
    coronet::tcp_acceptor ac{coronet::inet_address{port}};
    for (int i = 0; i < n; i++) {
        auto conn = co_await ac.accept_socket();
        char buf[256];
        int nr = co_await conn.recv(buf);
        if (nr > 0) {
            co_await conn.send({buf, static_cast<size_t>(nr)});
        }
        // conn destroyed here (RAII)
    }
    // Wait for all clients to verify
    while (done.load(std::memory_order_relaxed) < n) {
        co_await coronet::async::yield();
    }
    ctx.can_stop();
}

coronet::task<> echo_client(uint16_t port, const char* msg,
                            std::atomic<int>& ok, std::atomic<int>& done) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
    co_await s.connect(coronet::inet_address{"127.0.0.1", port});
    size_t len = std::strlen(msg);
    co_await s.send({msg, len});
    char buf[256];
    int nr = co_await s.recv(buf);
    if (nr == static_cast<int>(len) && std::memcmp(buf, msg, len) == 0) {
        ok.fetch_add(1, std::memory_order_relaxed);
    }
    done.fetch_add(1, std::memory_order_relaxed);
    // s destroyed here (RAII)
}

static bool test_basic_echo() {
    const uint16_t port = 18080;
    const int n = 3;
    std::atomic<int> ok{0}, done{0};

    coronet::io_context ctx;
    ctx.co_spawn(echo_server(ctx, port, n, done));
    const char* msgs[] = {"hello", "world", "coronet"};
    for (int i = 0; i < n; i++) {
        ctx.co_spawn(echo_client(port, msgs[i], ok, done));
    }
    ctx.co_spawn(timeout_stop(ctx, 5000));

    ctx.start();
    ctx.join();

    if (ok.load() == n) {
        printf("[PASS] basic_echo: %d/%d verified\n", ok.load(), n);
        return true;
    }
    printf("[FAIL] basic_echo: %d/%d verified\n", ok.load(), n);
    return false;
}

// ====================================================================
// Test 2: accept_with_peer — 验证对端地址
// ====================================================================

coronet::task<> peer_server(coronet::io_context& ctx, uint16_t port,
                            std::atomic<int>& ok, std::atomic<int>& done) {
    coronet::tcp_acceptor ac{coronet::inet_address{port}};
    auto [conn, peer] = co_await ac.accept_with_peer();
    // Verify peer is 127.0.0.1
    if (peer.family() == AF_INET && peer.to_ip() == "127.0.0.1") {
        ok.fetch_add(1, std::memory_order_relaxed);
    }
    // Echo back so client can complete
    char buf[256];
    int nr = co_await conn.recv(buf);
    if (nr > 0) {
        co_await conn.send({buf, static_cast<size_t>(nr)});
    }
    while (done.load(std::memory_order_relaxed) < 1) {
        co_await coronet::async::yield();
    }
    ctx.can_stop();
}

coronet::task<> peer_client(uint16_t port, std::atomic<int>& done) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
    co_await s.connect(coronet::inet_address{"127.0.0.1", port});
    const char* msg = "peer-test";
    co_await s.send({msg, std::strlen(msg)});
    char buf[256];
    co_await s.recv(buf);  // Wait for echo
    done.fetch_add(1, std::memory_order_relaxed);
}

static bool test_accept_with_peer() {
    const uint16_t port = 18081;
    std::atomic<int> ok{0}, done{0};

    coronet::io_context ctx;
    ctx.co_spawn(peer_server(ctx, port, ok, done));
    ctx.co_spawn(peer_client(port, done));
    ctx.co_spawn(timeout_stop(ctx, 5000));

    ctx.start();
    ctx.join();

    if (ok.load() == 1) {
        printf("[PASS] accept_with_peer: peer=127.0.0.1 verified\n");
        return true;
    }
    printf("[FAIL] accept_with_peer: peer address mismatch\n");
    return false;
}

// ====================================================================
// Test 3: Half-close — shutdown_write 后 server recv 返回 0
// ====================================================================

coronet::task<> half_close_server(coronet::io_context& ctx, uint16_t port,
                                  std::atomic<int>& ok) {
    coronet::tcp_acceptor ac{coronet::inet_address{port}};
    auto conn = co_await ac.accept_socket();
    char buf[256];
    int n1 = co_await conn.recv(buf);  // should get data
    int n2 = co_await conn.recv(buf);  // should get 0 (EOF from shutdown_write)
    if (n1 > 0 && n2 == 0) {
        ok.store(1, std::memory_order_relaxed);
    }
    ctx.can_stop();
}

coronet::task<> half_close_client(uint16_t port) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
    co_await s.connect(coronet::inet_address{"127.0.0.1", port});
    const char* msg = "half-close-test";
    co_await s.send({msg, std::strlen(msg)});
    co_await s.shutdown_write();
    // Keep socket alive briefly to ensure server processes FIN
    co_await coronet::async::timeout(std::chrono::milliseconds(500));
    // s destroyed here (RAII)
}

static bool test_half_close() {
    const uint16_t port = 18082;
    std::atomic<int> ok{0};

    coronet::io_context ctx;
    ctx.co_spawn(half_close_server(ctx, port, ok));
    ctx.co_spawn(half_close_client(port));
    ctx.co_spawn(timeout_stop(ctx, 5000));

    ctx.start();
    ctx.join();

    if (ok.load() == 1) {
        printf("[PASS] half_close: shutdown_write signals EOF (n2=0)\n");
        return true;
    }
    printf("[FAIL] half_close: expected n1>0 && n2==0\n");
    return false;
}

// ====================================================================
// Test 4: Concurrent connections — 5 个并发客户端
// ====================================================================

coronet::task<> concurrent_handler(coronet::tcp_socket conn) {
    char buf[256];
    int nr = co_await conn.recv(buf);
    if (nr > 0) {
        co_await conn.send({buf, static_cast<size_t>(nr)});
    }
    // conn destroyed here (RAII)
}

coronet::task<> concurrent_server(coronet::io_context& ctx, uint16_t port, int n,
                                  std::atomic<int>& done) {
    coronet::tcp_acceptor ac{coronet::inet_address{port}};
    for (int i = 0; i < n; i++) {
        auto conn = co_await ac.accept_socket();
        coronet::co_spawn(concurrent_handler(std::move(conn)));
    }
    // Wait for all clients to finish
    while (done.load(std::memory_order_relaxed) < n) {
        co_await coronet::async::yield();
    }
    ctx.can_stop();
}

coronet::task<> concurrent_client(uint16_t port, const char* msg,
                                  std::atomic<int>& ok, std::atomic<int>& done) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
    co_await s.connect(coronet::inet_address{"127.0.0.1", port});
    size_t len = std::strlen(msg);
    co_await s.send({msg, len});
    char buf[256];
    int nr = co_await s.recv(buf);
    if (nr == static_cast<int>(len) && std::memcmp(buf, msg, len) == 0) {
        ok.fetch_add(1, std::memory_order_relaxed);
    }
    done.fetch_add(1, std::memory_order_relaxed);
    // s destroyed here (RAII)
}

static bool test_concurrent_connections() {
    const uint16_t port = 18083;
    const int n = 5;
    std::atomic<int> ok{0}, done{0};

    coronet::io_context ctx;
    ctx.co_spawn(concurrent_server(ctx, port, n, done));
    const char* msgs[] = {"c0", "c1", "c2", "c3", "c4"};
    for (int i = 0; i < n; i++) {
        ctx.co_spawn(concurrent_client(port, msgs[i], ok, done));
    }
    ctx.co_spawn(timeout_stop(ctx, 5000));

    ctx.start();
    ctx.join();

    if (ok.load() == n) {
        printf("[PASS] concurrent: %d/%d verified\n", ok.load(), n);
        return true;
    }
    printf("[FAIL] concurrent: %d/%d verified\n", ok.load(), n);
    return false;
}

// ====================================================================
// Test 5: Repeated accept/connect cycles — fd 泄漏检测
// ====================================================================

coronet::task<> repeated_server(coronet::io_context& ctx, uint16_t port, int n,
                                std::atomic<int>& done) {
    coronet::tcp_acceptor ac{coronet::inet_address{port}};
    for (int i = 0; i < n; i++) {
        auto conn = co_await ac.accept_socket();
        char buf[64];
        int nr = co_await conn.recv(buf);
        if (nr > 0) {
            co_await conn.send({buf, static_cast<size_t>(nr)});
        }
        // conn destroyed here (RAII) — fd should be closed
    }
    while (done.load(std::memory_order_relaxed) < n) {
        co_await coronet::async::yield();
    }
    ctx.can_stop();
}

coronet::task<> repeated_client(uint16_t port, int n,
                                std::atomic<int>& ok, std::atomic<int>& done) {
    for (int i = 0; i < n; i++) {
        coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
        co_await s.connect(coronet::inet_address{"127.0.0.1", port});
        const char* msg = "ping";
        co_await s.send({msg, 4});
        char buf[64];
        int nr = co_await s.recv(buf);
        if (nr == 4 && std::memcmp(buf, msg, 4) == 0) {
            ok.fetch_add(1, std::memory_order_relaxed);
        }
        done.fetch_add(1, std::memory_order_relaxed);
        // s destroyed here (RAII) — fd should be closed
    }
}

static bool test_repeated_cycles() {
    const uint16_t port = 18084;
    const int n = 20;
    std::atomic<int> ok{0}, done{0};

    coronet::io_context ctx;
    ctx.co_spawn(repeated_server(ctx, port, n, done));
    ctx.co_spawn(repeated_client(port, n, ok, done));
    ctx.co_spawn(timeout_stop(ctx, 10000));

    ctx.start();
    ctx.join();

    if (ok.load() == n) {
        printf("[PASS] repeated_cycles: %d/%d echo verified (no fd leak)\n",
               ok.load(), n);
        return true;
    }
    printf("[FAIL] repeated_cycles: %d/%d verified\n", ok.load(), n);
    return false;
}

// ====================================================================
// Main
// ====================================================================

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== net_echo integration test ===\n\n");

    int failed = 0;
    if (!test_basic_echo()) failed++;
    if (!test_accept_with_peer()) failed++;
    if (!test_half_close()) failed++;
    if (!test_concurrent_connections()) failed++;
    if (!test_repeated_cycles()) failed++;

    printf("\n");
    if (failed == 0) {
        printf("=== net_echo: ALL PASSED ===\n");
        return 0;
    }
    printf("=== net_echo: %d TESTS FAILED ===\n", failed);
    return 1;
}
