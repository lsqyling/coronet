/// coronet channel test: 3 producers → 3 consumers
#include <coronet/coronet.hpp>
#include <cassert>
#include <cstdio>
using namespace coronet;
using namespace std::chrono_literals;

channel<std::string, 4> chan;
std::atomic<int> msg_consumed{0};
constexpr int TOTAL_MSGS = 12;
constexpr auto TEST_TIMEOUT = 8s;

task<> produce(std::string tag) {
    for (int i = 0; i < 2; ++i) {
        co_await chan.release(tag + ": fast produce");
    }
    for (int i = 0; i < 2; ++i) {
        co_await async::timeout(200ms);
        co_await chan.release(tag + ": slow produce");
    }
}

task<> consume(std::string tag) {
    while (msg_consumed.load(std::memory_order_relaxed) < TOTAL_MSGS) {
        std::string str{co_await chan.acquire()};
        int n = msg_consumed.fetch_add(1, std::memory_order_relaxed) + 1;
        printf("%s: %s (%d/%d)\n", tag.c_str(), str.c_str(), n, TOTAL_MSGS);
        fflush(stdout);
    }
}

task<> stopper(io_context& ctx) {
    co_await async::timeout(TEST_TIMEOUT);
    printf("timeout reached, consumed=%d\n", msg_consumed.load());
    ctx.can_stop();
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
    ctx.co_spawn(stopper(ctx));
    ctx.start();
    ctx.join();

    int got = msg_consumed.load();
    assert(got >= TOTAL_MSGS);
    printf("channel test PASSED (%d msgs)\n", got);
    return 0;
}
