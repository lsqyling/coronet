/// coronet netcat demo: TCP client/server with stdin/stdout relay
#include <coronet/coronet.hpp>

#include <cstring>
#include <string_view>

// Windows compat
#ifdef _WIN32
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#endif

using namespace coronet;
using Socket = coronet::socket;

// Receive from socket, write to stdout
task<> recv_session(Socket sock) {
    alignas(8192) char buf[8192];
    int nr = co_await sock.recv(buf);

    while (nr > 0) {
        nr = co_await (
            async::write(STDOUT_FILENO, {buf, (size_t)nr}, 0) && sock.recv(buf)
        );
    }
}

// Read from stdin, send to socket
task<> send_session(Socket sock) {
    alignas(8192) char buf[8192];
    int nr = co_await async::read(STDIN_FILENO, buf, 0);

    while (nr > 0) {
        nr = co_await (
            sock.send({buf, (size_t)nr}) && async::read(STDIN_FILENO, buf, 0)
        );
    }
}

task<> server(uint16_t port) {
    acceptor ac{inet_address{port}};
    for (int sockfd; (sockfd = co_await ac.accept()) >= 0;) {
        co_spawn(recv_session(Socket{sockfd}));
        co_spawn(send_session(Socket{sockfd}));
    }
}

task<> client(std::string_view hostname, uint16_t port) {
    inet_address addr;
    if (inet_address::resolve(hostname, port, addr)) {
        Socket sock{Socket::create_tcp(addr.family())};
        int res = co_await sock.connect(addr);
        if (res < 0) {
            printf("res=%d: %s\n", res, strerror(-res));
        }
        int fd = static_cast<int>(sock.native_handle());
        co_spawn(recv_session(Socket{fd}));
        co_spawn(send_session(Socket{fd}));
    } else {
        printf("Unable to resolve %s\n", hostname.data());
    }
}

int main(int argc, const char *argv[]) {
    if (argc < 3) {
        printf("Usage:\n  %s hostname port\n  %s -l port\n", argv[0], argv[0]);
        return 0;
    }

    io_context context;
    int port = atoi(argv[2]);
    if (strcmp(argv[1], "-l") == 0) {
        context.co_spawn(server(static_cast<uint16_t>(port)));
    } else {
        context.co_spawn(client(argv[1], static_cast<uint16_t>(port)));
    }

    context.start();
    context.join();
    return 0;
}
