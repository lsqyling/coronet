/// coronet TLS echo server example
/// Usage: tls_echo_server [port]
/// Requires: CORONET_WITH_TLS=ON + certificate/key files
#include <coronet/coronet.hpp>
#include <coronet/net/tls.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace coronet;

// TLS echo session — demonstrates using tls_socket with the transport concept
// 注意：conn 用右值引用而非按值 —— tls_socket 含 16KB raw_buf_，
// MSVC 2022 (14.41) 对协程按值传 ~16KB 大对象时的参数复制代码生成有 bug
// （目标地址被 32 位截断 → 访问冲突）。移入本地变量获取所有权。
task<> tls_session(tls_socket&& conn) {
    tls_socket sock = std::move(conn);
    char buf[8192];
    while (true) {
        int nr = co_await sock.recv(buf);
        if (nr <= 0) break;
        co_await sock.send({buf, static_cast<size_t>(nr)});
    }
}

task<> tls_server(uint16_t port, const tls_context& ctx) {
    tls_acceptor ac{inet_address{port}, ctx};
    printf("[TLS] listening on port %d\n", port);

    while (true) {
        try {
            auto conn = co_await ac.accept_socket();
            printf("[TLS] new connection\n");
            co_spawn(tls_session(std::move(conn)));
        } catch (const std::exception& e) {
            fprintf(stderr, "[TLS] accept error: %s\n", e.what());
        }
    }
}

int main(int argc, char** argv) {
    uint16_t port = 4443;
    const char* cert_file = "server.crt";
    const char* key_file = "server.key";

    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 2) cert_file = argv[2];
    if (argc > 3) key_file = argv[3];

    // Create server TLS context
    tls_context ctx{tls_context::mode::server};
    ctx.load_cert_file(cert_file, key_file);

    // Optional: set ALPN
    std::string_view alpn = "http/1.1";
    ctx.set_alpn({&alpn, 1});

    io_context ioc;
    ioc.co_spawn(tls_server(port, ctx));
    ioc.start();
    ioc.join();
    return 0;
}
