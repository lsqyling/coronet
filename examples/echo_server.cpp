/// coronet single-threaded echo server
#include <coronet/coronet.hpp>
#include <cstdlib>
using namespace coronet;

task<> session(int sockfd) {
    coronet::socket sock{sockfd};
    char buf[8192];

    while (true) {
        int nr = co_await sock.recv(buf);
        if (nr <= 0) break;
        co_await sock.send({buf, (size_t)nr});
    }
}

task<> server(const uint16_t port) {
    acceptor ac{inet_address{port}};
    for (int sock; (sock = co_await ac.accept()) >= 0;) {
        co_spawn(session(sock));
    }
}

int main(int argc, char **argv) {
    uint16_t port = 1234;
    if (argc > 1) port = (uint16_t)std::atoi(argv[1]);
    io_context ctx;
    ctx.co_spawn(server(port));
    ctx.start();
    ctx.join();
    return 0;
}
