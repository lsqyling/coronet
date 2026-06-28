/// coronet condition_variable notify_one demo (worker/main pattern)
#include <coronet/all.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <random>

using namespace coronet;
using namespace std::literals;

mutex m;
condition_variable cv;
std::string data;
bool ready = false;
bool processed = false;

task<void> worker_thread() {
    co_await m.lock();
    co_await cv.wait(m, [] { return ready; });

    std::cout << "Worker thread is processing data\n";
    data += " after processing";

    processed = true;
    std::cout << "Worker thread signals data processing completed\n";

    m.unlock();
    cv.notify_one();
}

task<> main_thread() {
    data = "Example data";
    {
        co_await m.lock();
        ready = true;
        std::cout << "main() signals data ready for processing\n";
    }
    cv.notify_one(); // fake notify

    char s[4];
    printf("input a char to continue:");
    [[maybe_unused]] int _ = scanf("%s", s);
    m.unlock();
    cv.notify_one(); // real notify

    {
        auto lk = co_await m.lock_guard();
        co_await cv.wait(m, [] { return processed; });
    }
    std::cout << "Back in main(), data = " << data << '\n';
}

int main() {
    io_context ctx;
    ctx.co_spawn(worker_thread());
    ctx.co_spawn(main_thread());
    ctx.start();
    ctx.join();
    return 0;
}
