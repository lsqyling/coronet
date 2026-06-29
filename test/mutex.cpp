/// coronet mutex stress test: 协程互斥锁正确性验证
/// 验证 100 个协程在互斥锁保护下累加共享计数器，结果应等于总操作数。
#include <coronet/coronet.hpp>
#include <cassert>
#include <atomic>

using namespace coronet;

constexpr int N_COROS = 100;
constexpr int N_INCR  = 10000;   // 每协程累加次数 / increments per coroutine

mutex mtx;
int cnt = 0;
std::atomic<int> done{0};

task<> add() {
    auto lock = co_await mtx.lock_guard();
    for (int i = 0; i < N_INCR; ++i) {
        ++cnt;
    }
    done.fetch_add(1, std::memory_order_relaxed);
}

/// 监控协程：等待所有 add() 完成后停止事件循环
/// Monitor: stop the event loop when all coroutines are done.
task<> monitor(io_context& ctx) {
    while (done.load(std::memory_order_relaxed) < N_COROS) {
        co_await async::yield();
    }
    ctx.can_stop();
}

int main() {
    io_context ctx;

    for (int i = 0; i < N_COROS; ++i) {
        ctx.co_spawn(add());
    }
    ctx.co_spawn(monitor(ctx));

    ctx.start();
    ctx.join();

    int expected = N_COROS * N_INCR;
    if (cnt == expected) {
        printf("mutex test PASSED: cnt=%d (expected %d)\n", cnt, expected);
        return 0;
    } else {
        fprintf(stderr, "mutex test FAILED: cnt=%d (expected %d)\n", cnt, expected);
        return 1;
    }
}
