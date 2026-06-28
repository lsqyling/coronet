/// coronet timer accuracy test
#include <chrono>
#include <coronet/io_context.hpp>
#include <coronet/async_io.hpp>
using namespace coronet;

task<> cycle_abs(int sec) {
    auto next = std::chrono::steady_clock::now();
    while (true) {
        next = next + std::chrono::seconds{sec};
        co_await async::timeout_at(next);
        auto late = std::chrono::steady_clock::now() - next;
        printf("late = %ld ns\n", late.count());
    }
}

int main() {
    io_context ctx;
    ctx.co_spawn(cycle_abs(1));
    ctx.start();
    ctx.join();
    return 0;
}
