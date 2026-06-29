/// coronet condition_variable notify_one test (worker/main pattern)
#include <coronet/all.hpp>
#include <atomic>
#include <cstddef>
#include <cstdio>

using namespace coronet;
using namespace std::literals;

mutex m;
condition_variable cv;
std::string data;
bool ready = false;
bool processed = false;
std::atomic<int> steps_done{0};

task<void> worker_thread() {
    {
        auto lk = co_await m.lock_guard();
        co_await cv.wait(m, [] { return ready; });
    }
    printf("Worker is processing data\n"); fflush(stdout);
    data += " after processing";
    processed = true;
    printf("Worker signals processing completed\n"); fflush(stdout);
    cv.notify_one();
    steps_done.fetch_add(1, std::memory_order_relaxed);
}

task<> main_thread() {
    data = "Example data";
    co_await async::timeout(200ms);

    {
        auto lk = co_await m.lock_guard();
        ready = true;
        printf("main() signals data ready\n"); fflush(stdout);
    }
    cv.notify_one();

    {
        auto lk = co_await m.lock_guard();
        co_await cv.wait(m, [] { return processed; });
    }
    printf("Back in main(), data = %s\n", data.c_str()); fflush(stdout);
    steps_done.fetch_add(1, std::memory_order_relaxed);
}

task<> stopper(io_context& ctx) {
    co_await async::timeout(5s);
    printf("timeout reached, steps_done=%d\n", steps_done.load());
    ctx.can_stop();
}

int main() {
    io_context ctx;
    ctx.co_spawn(worker_thread());
    ctx.co_spawn(main_thread());
    ctx.co_spawn(stopper(ctx));
    ctx.start();
    ctx.join();

    int done = steps_done.load();
    printf("cv_notify_one test: %d/2 steps completed\n", done);
    assert(done == 2);
    printf("cv_notify_one test PASSED\n");
    return 0;
}
