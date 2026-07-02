/**
 * when_all.cpp —— coronet when_all 组合器测试
 *
 * 测试目的：
 *   验证 coronet 的 all() 组合器能够同时启动多个协程，等待它们全部完成后
 *   通过结构化绑定一次性获取所有返回值。这是协程并发中"等待所有任务完成"
 *  （类似 std::when_all）的核心功能。
 *
 * 测试模式：
 *   - 定义一个 task<int>（1 秒延迟）、一个 shared_task<string>（立即完成）
 *     和一个 task<void>（2 秒延迟）
 *   - 使用 all(f0(), f1(), f2()) 并发运行三个协程
 *   - 通过结构化绑定 [r0, r1] 获取 f0 和 f1 的返回值
 *   - f2 返回 void，不参与结构化绑定
 *
 * 关键验证点：
 *   - all() 能同时调度不同类型的协程（task<int>、shared_task<string>、task<void>）
 *   - 所有任务即使延迟不同，也能正确等待全部完成
 *   - 结构化绑定能正确解包返回值
 *   - assert 验证 r0 == 1、r1 == "f1 Great!"
 *   - task<void> 类型的协程不产生返回值，不参与结构化绑定
 *
 * 涉及概念：all() 组合器、结构化绑定、task、shared_task、混合类型并发
 */

/// coronet when_all test: 等待所有任务完成，通过结构化绑定获取结果
/// Wait for all tasks, get results via structured binding.
#include <coronet/all.hpp>
#include <cassert>
#include <cstdio>
using namespace coronet;

/**
 * f0——1 秒后返回整数 1 的 task<int>
 *
 * 使用 async::timeout 模拟异步延迟，验证 all() 能等待有延迟的任务。
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
 * 使用 shared_task 而非 task，说明 all() 能接受不同所有权语义的 task 类型。
 * shared_task 支持多等待者（multi-waiter），但在这里只被 all() 等待一次。
 * 立即完成（无 co_await 延迟）用于验证组合器对即时完成任务的正确处理。
 */
shared_task<std::string> f1() {
    printf("f1 start.\n"); fflush(stdout);
    printf("f1 done.\n"); fflush(stdout);
    co_return "f1 Great!";
}

/**
 * f2——2 秒后返回 void 的 task<void>
 *
 * 返回 void 类型的协程，验证 all() 能处理无返回值的任务。
 * 该任务不产生结构化绑定变量，只用于等待其完成。
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
 * 使用 all() 同时启动三个协程，等待全部完成后验证返回值。
 * 所有任务中 f2 延迟最长（2 秒），所以 all() 大约需要 2 秒完成。
 * 完成后调用 can_stop() 通知事件循环可以退出。
 */
task<> run_and_stop(io_context& ctx) {
    auto [r0, r1] = co_await all(f0(), f1(), f2());
    // 验证结果 / Verify results
    assert(r0 == 1);
    assert(r1 == "f1 Great!");
    printf("get the result of f0: %d\n", r0);
    printf("get the result of f1: %s\n", r1.c_str());
    printf("when_all test PASSED\n");
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
