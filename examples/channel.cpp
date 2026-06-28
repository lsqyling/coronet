/// coronet channel demo: 3 producers → 3 consumers (single io_context)
#include <coronet/coronet.hpp>
#include <cstdio>
using namespace coronet;
using namespace std::chrono_literals;

channel<std::string, 4> chan;  // buffered: avoids rendezvous deadlock with timeout

task<> produce(std::string tag) {
    constexpr int repeat = 2;
    for (;;) {
        for (int i = 0; i < repeat; ++i) {
            co_await chan.release(tag + ": fast produce");
        }
        for (int i = 0; i < repeat; ++i) {
            co_await async::timeout(1s);
            co_await chan.release(tag + ": slow produce");
        }
    }
}

task<> consume(std::string tag) {
    for (;;) {
        std::string str{co_await chan.acquire()};
        printf("%s: %s\n", tag.c_str(), str.c_str()); fflush(stdout);
        co_await async::timeout(200ms);
    }
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    io_context ctx;
    ctx.co_spawn(produce("p0"));
    ctx.co_spawn(produce("p1"));
    ctx.co_spawn(produce("p2"));
    ctx.co_spawn(consume("c0"));
    ctx.co_spawn(consume("c1"));
    ctx.co_spawn(consume("c2"));
    ctx.start();
    ctx.join();
    return 0;
}
