/// coronet mutex stress test: 1000 coroutines incrementing a shared counter
#include <coronet/coronet.hpp>
#include <iostream>

using namespace coronet;
mutex mtx;
int cnt = 0;

task<> add() {
    auto lock = co_await mtx.lock_guard();
    for (int i = 0; i < 1000000; ++i) {
        ++cnt;
    }
    std::cout << cnt << std::endl;
}

int main() {
    constexpr int N = 10;
    io_context ctx[N];
    for (int i = 0; i < 1000; ++i) {
        ctx[i % N].co_spawn(add());
    }
    for (auto &c : ctx) { c.start(); }
    ctx[0].join(); // never stop
    return 0;
}
