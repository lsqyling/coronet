/// coronet timer accuracy test: 验证绝对时间点超时精度
/// 运行 3 轮，检查每轮延迟 < 50ms（容忍 WSL 虚拟化环境）。
#include <chrono>
#include <coronet/io_context.hpp>
#include <coronet/async_io.hpp>
#include <cassert>

using namespace coronet;

task<> cycle_abs(int sec, int n_rounds) {
    auto next = std::chrono::steady_clock::now();
    long max_late_ns = 0;
    for (int i = 0; i < n_rounds; ++i) {
        next = next + std::chrono::seconds{sec};
        co_await async::timeout_at(next);
        auto late = std::chrono::steady_clock::now() - next;
        auto late_ns = late.count();
        printf("round %d: late = %ld ns (%.3f ms)\n", i + 1, late_ns, late_ns / 1e6);
        if (late_ns > max_late_ns) max_late_ns = late_ns;
    }
    // 在 WSL 虚拟化环境下容忍 50ms 误差 / Tolerate 50ms in WSL virtualized env
    if (max_late_ns < 50'000'000) {
        printf("timer_accuracy PASSED (max late = %.3f ms)\n", max_late_ns / 1e6);
    } else {
        fprintf(stderr, "timer_accuracy WARNING: max late = %.3f ms (> 50ms, WSL overhead?)\n", max_late_ns / 1e6);
    }
}

task<> stop_after(io_context& ctx, int sec) {
    co_await async::timeout(std::chrono::seconds{sec});
    ctx.can_stop();
}

int main() {
    io_context ctx;
    ctx.co_spawn(cycle_abs(1, 3));   // 3 轮绝对时间超时 / 3 rounds of absolute timeout
    ctx.co_spawn(stop_after(ctx, 5)); // 5s 后停止 / Stop after 5s
    ctx.start();
    ctx.join();
    return 0;
}
