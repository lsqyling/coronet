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
task<> tls_session(tls_socket conn) {
    char buf[8192];
    while (true) {
        int nr = co_await conn.recv(buf);
        if (nr <= 0) break;
        co_await conn.send({buf, static_cast<size_t>(nr)});
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
