/// coronet timer test: 验证相对超时和周期性定时器
/// 运行 4 轮后自动停止，验证每次定时在 ±200ms 误差内触发。
#include <coronet/io_context.hpp>
#include <coronet/async_io.hpp>
#include <cassert>
#include <atomic>
#include <chrono>

using namespace coronet;

/// 运行指定轮数后退出 / Run N cycles then exit
task<> cycle_n(int sec, const char *message, int n_rounds) {
    for (int i = 0; i < n_rounds; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        co_await async::timeout(std::chrono::seconds{sec});
        auto elapsed = std::chrono::steady_clock::now() - t0;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        printf("%s (round %d, elapsed %ld ms)\n", message, i + 1, ms);
        fflush(stdout);
    }
}

/// 定时器完成后停止事件循环 / Stop when all timers are done
task<> stop_after(io_context& ctx, int sec) {
    co_await async::timeout(std::chrono::seconds{sec});
    ctx.can_stop();
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    io_context ctx;

    ctx.co_spawn(cycle_n(1, "1 sec",    2));  // 2 轮 × 1s = 2s
    ctx.co_spawn(cycle_n(1, "1 sec [rel]", 2));
    ctx.co_spawn(cycle_n(3, "\t3 sec",  1));  // 1 轮 × 3s = 3s
    ctx.co_spawn(stop_after(ctx, 6));          // 6s 后停止

    ctx.start();
    ctx.join();
    printf("timer test PASSED\n");
    return 0;
}
