/**
 * cv_notify_all.cpp —— coronet 条件变量 notify_all 测试
 *
 * 测试目的：
 *   验证 coronet 的 condition_variable 在 notify_all 模式下的行为。
 *   多个等待协程在同一个条件变量上等待，当条件满足并被 notify_all 唤醒时，
 *   所有等待者都应能继续执行。这与 notify_one（只唤醒一个）形成对比。
 *
 * 测试模式：
 *   - 3 个 waiter 协程在条件变量 cv 上等待条件 i == 1
 *   - signals 协程分两阶段执行：
 *     第一阶段（300ms 后）：notify_all()，但 i 仍为 0，等待者检查条件后继续等待
 *     第二阶段（再 300ms 后）：设置 i = 1，再次 notify_all()，等待者被唤醒
 *   - 所有等待者通过条件检查后各自完成
 *   - stopper 协程在 5 秒超时后停止事件循环
 *
 * 关键验证点：
 *   - notify_all() 能唤醒所有等待在同一条件变量上的协程
 *   - 条件变量 wait 使用谓词（predicate）时，即使被 notify 也会重新检查条件
 *     （第一阶段 notify 时 i == 0，所有等待者检查谓词后发现不满足，继续等待）
 *   - 设置 i = 1 后 notify_all，所有 3 个等待者都通过谓词检查并继续执行
 *   - 锁（mutex）在 wait 期间的正确释放和重获取
 *   - waiters_done == 3 验证所有 3 个等待者都完成了
 *
 * 涉及概念：condition_variable、wait（带谓词）、notify_all、spurious wakeup 防护
 */

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

/**
 * waits——等待条件变量满足的协程（共 3 个）
 *
 * 每个 waiter 获取锁后调用 cv.wait(cv_m, [] { return i == 1; })。
 * wait 内部会释放锁、阻塞等待通知、被唤醒后重获锁、再检查谓词。
 * 如果谓词不满足（i != 1），即使被 notify 也会继续等待。
 * 这符合"带谓词的 wait 防止虚假唤醒"的标准模式。
 */
task<> waits() {
    auto lk = co_await cv_m.lock_guard();
    printf("Waiting...\n"); fflush(stdout);
    co_await cv.wait(cv_m, [] { return i == 1; });
    printf("...finished waiting. i == 1\n"); fflush(stdout);
    waiters_done.fetch_add(1, std::memory_order_relaxed);
}

/**
 * signals——控制条件变量状态的信号协程
 *
 * 分两阶段唤醒：
 *   阶段 1（300ms）：在没有修改条件 i 的情况下 notify_all()。
 *     这测试了"notify 但条件不满足"的行为——等待者被唤醒后检查谓词，
 *     发现 i != 1，于是继续等待。
 *   阶段 2（再 300ms）：设置 i = 1 后 notify_all()。
 *     此时谓词满足，所有等待者都能通过检查并继续执行。
 */
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

/**
 * stopper——超时停止的安全兜底
 */
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
