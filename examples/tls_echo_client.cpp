/// coronet TLS echo client example
/// Usage: tls_echo_client <host> <port> [message]
/// Requires: CORONET_WITH_TLS=ON
#include <coronet/coronet.hpp>
#include <coronet/net/tls.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace coronet;

task<> tls_client(const char* host, uint16_t port, const char* msg) {
    inet_address addr;
    if (!inet_address::resolve(host, port, addr)) {
        fprintf(stderr, "[client] resolve failed: %s\n", host);
        co_return;
    }

    // Create client TLS context
    // NOTE: For production, enable peer verification:
    //   ctx.set_verify_peer(true);
    //   ctx.set_default_verify_paths();
    tls_context ctx{tls_context::mode::client};
    ctx.set_verify_peer(false);  // skip verification for demo

    // Optional: set ALPN
    std::string_view alpn = "http/1.1";
    ctx.set_alpn({&alpn, 1});

    // Connect + TLS handshake
    printf("[client] connecting to %s:%d (TLS)\n", host, port);
    auto sock = co_await tls_socket::connect(addr, ctx);
    printf("[client] TLS connected\n");

    auto negotiated = sock.negotiated_alpn();
    if (negotiated) {
        printf("[client] ALPN: %s\n", negotiated->c_str());
    }

    // Send message
    int ns = co_await sock.send({msg, std::strlen(msg)});
    printf("[client] sent %d bytes: '%s'\n", ns, msg);

    // Receive echo
    char buf[8192] = {};
    int nr = co_await sock.recv(buf);
    printf("[client] received %d bytes: '%.*s'\n", nr, nr, buf);

    co_await sock.shutdown_write();
    co_await sock.close();
    printf("[client] done\n");
}

int main(int argc, char** argv) {
    const char* host = "127.0.0.1";
    uint16_t port = 4443;
    const char* msg = "Hello, TLS!";

    if (argc > 1) host = argv[1];
    if (argc > 2) port = static_cast<uint16_t>(std::atoi(argv[2]));
    if (argc > 3) msg = argv[3];

    io_context ioc;
    ioc.co_spawn(tls_client(host, port, msg));
    ioc.start();
    ioc.join();
    return 0;
}
