// multi-threaded redis_echo_server
#include <coronet/coronet.hpp>

using namespace coronet;

constexpr uint16_t port = 6379;
constexpr uint32_t worker_num = 6;
static_assert(worker_num > 0);

task<> reply(coronet::socket sock) {
    char recv_buf[100];
    int n = co_await sock.recv(recv_buf);
    while (n > 0) {
        n = co_await (sock.send({"+PONG\r\n", 7}) && sock.recv(recv_buf));
    }
}

task<> server(io_context workers[]) {
    acceptor ac{inet_address{port}};
    uint32_t turn = 0;
    for (int sockfd; (sockfd = co_await ac.accept()) >= 0;) {
        workers[turn].co_spawn(reply(coronet::socket{sockfd}));
        turn = (turn + 1) % worker_num;
    }
}

int main() {
    io_context workers[worker_num];
    io_context balancer;

    balancer.co_spawn(server(workers));
    balancer.start();
    for (auto& ctx : workers) {
        ctx.start();
    }
    balancer.join(); // never stop
    return 0;
}
