/// coronet condition_variable notify_all demo
#include <coronet/all.hpp>

#include <array>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <random>

coronet::condition_variable cv;
coronet::mutex cv_m;
int i = 0;

using namespace coronet;
using namespace std::chrono_literals;

task<> waits() {
    auto lk = co_await cv_m.lock_guard();
    std::cerr << "Waiting... \n";
    co_await cv.wait(cv_m, [] { return i == 1; });
    std::cerr << "...finished waiting. i == 1\n";
}

task<> signals() {
    co_await async::timeout(1s);

    {
        auto lk = co_await cv_m.lock_guard();
        std::cerr << "Notifying...\n";
    }
    cv.notify_all();

    co_await async::timeout(1s);

    {
        auto lk = co_await cv_m.lock_guard();
        i = 1;
        std::cerr << "Notifying again...\n";
    }
    cv.notify_all();
}

int main() {
    io_context ctx;
    ctx.co_spawn(waits());
    ctx.co_spawn(waits());
    ctx.co_spawn(waits());
    ctx.co_spawn(signals());
    ctx.start();
    ctx.join();
    return 0;
}
