/// Integration test: TCP echo server using coronet coroutine APIs.
/// Starts a server on a port, connects a client, sends data,
/// and verifies the echo response.

#include <coronet/coronet.hpp>
#include <coronet/io_context.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>

// Note: avoid 'using namespace coronet' because <sys/socket.h> (pulled in
// via io_uring headers) defines a ::socket() function that shadows the class.

constexpr uint16_t TestPort = 9090;
constexpr const char* TestMsg = "Hello, coronet echo server!";
constexpr int BufSize = 4096;

// ---- Echo server session ----
coronet::task<> echo_session(int sockfd) {
    coronet::socket sock{sockfd};
    char buf[BufSize];

    while (true) {
        int nr = co_await sock.recv(buf);
        if (nr <= 0) {
            std::printf("  [server] recv returned %d, closing session\n", nr);
            break;
        }
        int ns = co_await sock.send({buf, static_cast<size_t>(nr)});
        if (ns <= 0) {
            std::printf("  [server] send returned %d, closing session\n", ns);
            break;
        }
    }
}

// ---- Echo server (accepts one connection then exits) ----
coronet::task<> echo_server() {
    coronet::acceptor ac{coronet::inet_address{TestPort}};
    std::printf("[server] listening on port %d\n", TestPort);

    // Accept just one connection for the test
    int sock = co_await ac.accept();
    if (sock >= 0) {
        std::printf("[server] accepted connection, fd=%d\n", sock);
        co_spawn(echo_session(sock));
    }
}

// ---- Echo client ----
// Note: receives io_context reference so it can call can_stop() when done.
coronet::task<> echo_client(bool& success, coronet::io_context& ctx) {
    coronet::inet_address addr;
    bool resolved = coronet::inet_address::resolve("127.0.0.1", TestPort, addr);
    if (!resolved) {
        std::fprintf(stderr, "[client] resolve failed\n");
        success = false;
        ctx.can_stop();
        co_return;
    }

    coronet::socket sock{coronet::socket::create_tcp(addr.family())};
    std::printf("[client] connecting to 127.0.0.1:%d\n", TestPort);

    int conn_res = co_await sock.connect(addr);
    if (conn_res < 0) {
        std::fprintf(stderr, "[client] connect failed: %d\n", conn_res);
        success = false;
        ctx.can_stop();
        co_return;
    }
    std::printf("[client] connected\n");

    // Send test message
    int ns = co_await sock.send({TestMsg, std::strlen(TestMsg)});
    std::printf("[client] sent %d bytes\n", ns);
    assert(ns > 0);

    // Receive echo
    char buf[BufSize] = {};
    int nr = co_await sock.recv(buf);
    std::printf("[client] received %d bytes: '%.*s'\n", nr, nr, buf);
    assert(nr == ns);
    assert(std::strncmp(buf, TestMsg, nr) == 0);

    // Shutdown write side
    co_await sock.shutdown_write();

    success = true;
    std::printf("[client] echo test passed!\n");

    // Signal the event loop to stop — we are done
    ctx.can_stop();
}

int main() {
    std::printf("=== coronet Echo Server Integration Test ===\n");

    bool success = false;

    // Start echo server in background thread
    coronet::io_context server_ctx;
    server_ctx.co_spawn(echo_server());
    server_ctx.start();

    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Run client — echo_client calls can_stop() when done
    coronet::io_context client_ctx;
    client_ctx.co_spawn(echo_client(success, client_ctx));
    client_ctx.start();
    client_ctx.join();

    // Stop server
    server_ctx.can_stop();
    server_ctx.join();

    if (success) {
        std::printf("=== Echo Server Integration Test PASSED ===\n");
        return 0;
    } else {
        std::fprintf(stderr, "=== Echo Server Integration Test FAILED ===\n");
        return 1;
    }
}
