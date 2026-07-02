/**
 * when_some.cpp —— coronet when_some 组合器测试
 *
 * 测试目的：
 *   验证 coronet 的 some(N, ...) 组合器能够同时启动多个协程，并在其中
 *   N 个任务完成后返回结果列表。这是 all()（等全部）和 any()（等最先）
 *   之间的折中——等部分任务完成即可继续。
 *
 * 测试模式：
 *   - 定义三个协程：f0（1 秒延迟返回 int）、f1（立即返回 string）、
 *     f2（2 秒延迟返回 void）
 *   - 使用 some(2, f0(), f1(), f2()) 等待任意 2 个完成
 *   - f1 立即完成，f0 在 1 秒后完成，此时 some() 返回 2 个结果
 *   - f2（最慢）被忽略
 *   - 遍历结果列表，使用 std::visit + overload 访问每个结果
 *
 * 关键验证点：
 *   - some(N) 在 N 个任务完成时立即返回，不等剩余任务
 *   - 返回的 vector 大小等于 N（此处 N=2）
 *   - 每个元素包含 (index, variant)，idx 标识任务编号
 *   - 最快完成的 2 个任务是 f1（立即）和 f0（1 秒后）
 *   - f2（2 秒后）被自动取消/丢弃
 *   - 使用 overload + std::visit 模式处理不同类型的返回值
 *
 * 涉及概念：some() 组合器、部分等待、variant、overload 模式
 */

/// coronet when_some test: 等待 N 个任务完成
/// Wait for N tasks to complete.
#include <coronet/all.hpp>
#include <cassert>
#include <variant>
#include <cstdio>
using namespace coronet;

/**
 * f0——1 秒后返回整数 0 的 task<int>
 *
 * 中等延迟，在 some(2, ...) 中应该是第二个完成的（在 f1 之后）。
 */
task<int> f0() {
    printf("f0 start.\n"); fflush(stdout);
    co_await async::timeout(std::chrono::seconds{1});
    printf("f0 done.\n"); fflush(stdout);
    co_return 0;
}

/**
 * f1——立即返回字符串的 shared_task<string>
 *
 * 无延迟，立即完成，在 some(2, ...) 中必然是第一个完成的。
 */
shared_task<std::string> f1() {
    printf("f1 start.\n"); fflush(stdout);
    printf("f1 done.\n"); fflush(stdout);
    co_return "f1 Great!";
}

/**
 * f2——2 秒后返回 void 的 task<void>
 *
 * 延迟最长，在 some(2, ...) 中不会进入前两名，因此被忽略。
 * 验证组合器能否正确丢弃最慢的任务。
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
 * 使用 some(2, f0(), f1(), f2()) 等待任意 2 个任务完成。
 * 预期结果：f1 立即完成 + f0 在 1 秒后完成，此时 some() 返回。
 * results.size() 应为 2。遍历打印每个结果的索引和值。
 */
task<> run_and_stop(io_context& ctx) {
    auto results = co_await some(2, f0(), f1(), f2());
    // 应有 2 个结果（f1 立即完成 + f0 1s 后完成）/ Should have 2 results
    assert(results.size() == 2);
    for (const auto &[idx, var] : results) {
        printf("get the result of f%zu: ", idx);
        std::visit(
            overload{
                [](std::monostate) { printf("(void)\n"); },
                [](int x) { printf("%d : int\n", x); },
                [](const std::string &s) { printf("%s : string\n", s.c_str()); },
            },
            var
        );
    }
    printf("when_some test PASSED\n");
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
