/**
 * when_any.cpp —— coronet when_any 组合器测试
 *
 * 测试目的：
 *   验证 coronet 的 any() 组合器能够同时启动多个协程，并在第一个协程完成时
 *   立即返回其结果（返回 (index, variant) 对）。这类似于 Promise.race() 或
 *   std::when_any，用于"谁先完成就用谁的结果"的场景。
 *
 * 测试模式：
 *   - 定义三个协程：f0（1 秒延迟返回 int）、f1（立即返回 string）、
 *     f2（2 秒延迟返回 void）
 *   - 使用 any(f0(), f1(), f2()) 并发运行
 *   - f1 立即完成，所以胜者是索引为 1 的 f1
 *   - 通过 std::visit + overload 模式访问 variant 中的结果
 *
 * 关键验证点：
 *   - any() 在第一个任务完成时立即返回，不等其他任务
 *   - 返回的 idx 指向最先完成的任务（f1，idx == 1）
 *   - variant 包含该任务的返回值（f1 返回 string）
 *   - 未完成的任务被自动取消/丢弃
 *   - 使用 overload + std::visit 模式处理不同的返回值类型
 *   - std::monostate 处理 void 类型返回值的情况
 *
 * 涉及概念：any() 组合器、variant、std::visit、overload 模式、竞速模式
 */

/// coronet when_any test: 首个完成者胜出，返回 (index, variant)
/// First completion wins, returns (index, variant).
#include <coronet/all.hpp>
#include <cassert>
#include <variant>
#include <cstdio>
using namespace coronet;

/**
 * f0——1 秒后返回整数 1 的 task<int>
 *
 * 有 1 秒延迟，因此不会是首个完成者（除非其他任务更慢）。
 */
task<int> f0() {
    printf("f0 start.\n"); fflush(stdout);
    co_await async::timeout(std::chrono::seconds{1});
    printf("f0 done.\n"); fflush(stdout);
    co_return 1;
}

/**
 * f1——立即返回字符串的 shared_task<string>
 *
 * 没有延迟，立即完成，因此必定是 any() 的胜者。
 * 验证 any() 对即时完成任务的处理。
 */
shared_task<std::string> f1() {
    printf("f1 start.\n"); fflush(stdout);
    printf("f1 done.\n"); fflush(stdout);
    co_return "f1 Great!";
}

/**
 * f2——2 秒后返回 void 的 task<void>
 *
 * 延迟最长（2 秒），因此不会胜出。
 * 验证 any() 能否在胜者出现后正确丢弃/取消尚未完成的任务。
 */
task<void> f2() {
    printf("f2 start.\n"); fflush(stdout);
    co_await async::timeout(std::chrono::seconds{2});
    printf("f2 done.\n"); fflush(stdout);
    co_return;
}

/**
 * run_and_stop——主测试协程
 *
 * 使用 any() 并发运行三个协程。由于 f1 立即完成，any() 应返回 idx == 1
 * 以及包含字符串 "f1 Great!" 的 variant。
 * 使用 std::visit 和 overload 访问 variant 中的不同可能类型。
 */
task<> run_and_stop(io_context& ctx) {
    auto [idx, var] = co_await any(f0(), f1(), f2());
    // f1 立即完成，应为胜者 / f1 completes immediately, should win
    assert(idx == 1);
    printf("get the result of f%u: ", idx);
    std::visit(
        overload{
            [](std::monostate) { printf("(void)\n"); },
            [](int x) { printf("%d : int\n", x); },
            [](const std::string &s) { printf("%s : string\n", s.c_str()); },
        },
        var
    );
    printf("when_any test PASSED\n");
    ctx.can_stop();
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    io_context ctx;
    ctx.co_spawn(run_and_stop(ctx));
    ctx.start();
    ctx.join();
    return 0;
}
