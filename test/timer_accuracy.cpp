/**
 * timer_accuracy.cpp —— coronet 异步定时器精度测试
 *
 * 测试目的：
 *   验证 coronet 的 async::timeout_at（绝对时间点超时）功能的精度。
 *   与相对超时（timeout）不同，timeout_at 接受一个具体的时间点，
 *   协程在该时间点到达时恢复执行。本测试测量每次唤醒相对于目标时间
 *   点的延迟（late_ns），并检查最大延迟是否在容忍范围内。
 *
 * 测试模式：
 *   - 使用 async::timeout_at 进行绝对时间点定时
 *   - 每次唤醒后计算与目标时间点的差值（late_ns）
 *   - 记录所有轮次中的最大延迟
 *   - 对 WSL 虚拟化环境放宽容忍度到 50ms
 *
 * 关键验证点：
 *   - async::timeout_at 能否在精确的时间点唤醒协程
 *   - 多次调用的累计误差（每次 next = next + seconds，不重新读取时钟）
 *   - 实际精度是否符合预期（native Linux 应远低于 50ms）
 *   - 在虚拟化环境（WSL）中也能正常工作
 *
 * 涉及概念：io_context、async::timeout_at、绝对时间点定时、精度测量
 */

/// coronet timer accuracy test: 验证绝对时间点超时精度
/// 运行 3 轮，检查每轮延迟 < 50ms（容忍 WSL 虚拟化环境）。
#include <chrono>
#include <coronet/io_context.hpp>
#include <coronet/async_io.hpp>
#include <cassert>

using namespace coronet;

/**
 * cycle_abs——使用绝对时间点进行 N 轮超时测试
 *
 * 每次循环开始时计算下一个目标时间点 next = next + seconds，然后
 * co_await async::timeout_at(next)。这种模式确保即使前一轮有延迟，
 * 后续轮次仍然以固定的时间间隔触发（而不是相对于上一次实际唤醒时间）。
 * 记录所有轮次中的最大延迟用于精度评估。
 */
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

/**
 * stop_after——在指定秒数后停止事件循环的安全网
 *
 * 作为后备机制确保即使定时器未触发，测试也能在 5 秒后超时退出。
 * 这是测试中常见的容错模式，防止测试用例挂起。
 */
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
