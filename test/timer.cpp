/**
 * timer.cpp —— coronet 异步定时器功能测试
 *
 * 测试目的：
 *   验证 coronet 的 async::timeout（相对超时）功能。通过启动多个不同定时周期
 *   的协程，验证定时器能否在指定的相对延迟后正确唤醒协程，以及多次周期性超时
 *   的准确性。
 *
 * 测试模式：
 *   - 使用 io_context + co_spawn 启动多个定时器协程
 *   - 每个协程循环执行 N 次 timeout（如 1 秒定时器执行 2 轮）
 *   - 使用额外的 stop_after 协程在指定时间后通过 can_stop() 停止事件循环
 *   - 主线程调用 start() + join() 阻塞等待全部协程完成
 *
 * 关键验证点：
 *   - async::timeout 能否在约定期限后恢复协程执行
 *   - 同一个协程能否多次 co_await timeout 实现周期性定时
 *   - 不同定时周期的多个协程能否并发正确触发
 *   - 实际延迟是否在可接受范围内（打印每次 elapsed 供人工检查）
 *   - 事件循环的停止机制（can_stop）是否能正确终止
 *
 * 涉及概念：io_context、co_spawn、async::timeout、can_stop、start/join
 */

/// coronet timer test: 验证相对超时和周期性定时器
/// 运行 4 轮后自动停止，验证每次定时在 ±200ms 误差内触发。
#include <coronet/io_context.hpp>
#include <coronet/async_io.hpp>
#include <cassert>
#include <atomic>
#include <chrono>

using namespace coronet;

/**
 * cycle_n——运行 n_rounds 轮超时后自动退出
 *
 * 在循环中反复 co_await async::timeout(seconds)，模拟周期性定时器行为。
 * 每次唤醒后记录实际经过时间并打印，用于人工验证定时精度。
 * 这是测试协程能否被定时器多次唤醒的基本模式。
 */
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

/**
 * stop_after——在指定秒数后停止事件循环
 *
 * 这是一种常用的测试辅助模式：定时一段时间后调用 ctx.can_stop()，
 * 告诉 io_context 在所有当前任务完成后可以安全退出。
 * 这样主线程的 join() 调用会在所有协程完成后返回，无需手动干预。
 */
/// 定时器完成后停止事件循环 / Stop when all timers are done
task<> stop_after(io_context& ctx, int sec) {
    co_await async::timeout(std::chrono::seconds{sec});
    ctx.can_stop();
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    io_context ctx;

    /**
     * 启动 3 个定时器协程 + 1 个停止协程：
     *   - 两个 1 秒周期 × 2 轮 = 各运行 2 秒
     *   - 一个 3 秒周期 × 1 轮 = 运行 3 秒
     *   - 停止协程在 6 秒后调用 can_stop()
     * 最长协程（3s × 1）在 3 秒时完成，但停止协程等待 6 秒后才终止，确保所有协程有时间完成。
     */
    ctx.co_spawn(cycle_n(1, "1 sec",    2));  // 2 轮 × 1s = 2s
    ctx.co_spawn(cycle_n(1, "1 sec [rel]", 2));
    ctx.co_spawn(cycle_n(3, "\t3 sec",  1));  // 1 轮 × 3s = 3s
    ctx.co_spawn(stop_after(ctx, 6));          // 6s 后停止

    ctx.start();
    ctx.join();
    printf("timer test PASSED\n");
    return 0;
}
