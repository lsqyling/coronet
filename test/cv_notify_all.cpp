/// coronet condition_variable notify_all test
#include <coronet/all.hpp>
#include <atomic>
#include <cstddef>
#include <cstdio>

using namespace coronet;
using namespace std::chrono_literals;

mutex cv_m;
condition_variable cv;
int i = 0;
std::atomic<int> waiters_done{0};

task<> waits() {
    auto lk = co_await cv_m.lock_guard();
    printf("Waiting...\n"); fflush(stdout);
    co_await cv.wait(cv_m, [] { return i == 1; });
    printf("...finished waiting. i == 1\n"); fflush(stdout);
    waiters_done.fetch_add(1, std::memory_order_relaxed);
}

task<> signals() {
    co_await async::timeout(300ms);
    {
        auto lk = co_await cv_m.lock_guard();
        printf("Notifying...\n"); fflush(stdout);
    }
    cv.notify_all();

    co_await async::timeout(300ms);
    {
        auto lk = co_await cv_m.lock_guard();
        i = 1;
        printf("Notifying again (i=1)...\n"); fflush(stdout);
    }
    cv.notify_all();
}

task<> stopper(io_context& ctx) {
    co_await async::timeout(5s);
    printf("timeout reached, waiters_done=%d\n", waiters_done.load());
    ctx.can_stop();
}

int main() {
    io_context ctx;
    ctx.co_spawn(waits());
    ctx.co_spawn(waits());
    ctx.co_spawn(waits());
    ctx.co_spawn(signals());
    ctx.co_spawn(stopper(ctx));
    ctx.start();
    ctx.join();

    int done = waiters_done.load();
    printf("cv_notify_all test: %d/3 waiters completed\n", done);
    assert(done == 3);
    printf("cv_notify_all test PASSED\n");
    return 0;
}
