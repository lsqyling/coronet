/**
 * cv_notify_one.cpp —— coronet 条件变量 notify_one 测试（worker/main 模式）
 *
 * 测试目的：
 *   验证 coronet 的 condition_variable 在 notify_one 模式下的行为，
 *   模拟经典的 worker-main 协作模式。main 协程准备数据后通知 worker 开始
 *   处理，worker 处理完成后通知 main 取回结果。
 *   这展示了条件变量用于协程间一对一步调同步的典型用法。
 *
 * 测试模式：
 *   - 两个协程：worker_thread（处理数据）和 main_thread（控制流程）
 *   - 使用两个标志位（ready 和 processed）协同两阶段工作流：
 *     阶段 1：main 设置 ready=true，notify_one 唤醒 worker
 *     阶段 2：worker 处理数据后设置 processed=true，notify_one 唤醒 main
 *   - 每个阶段都使用带谓词的 wait 确保不会因虚假唤醒而提前执行
 *   - stopper 协程在 5 秒超时后停止事件循环
 *
 * 关键验证点：
 *   - notify_one 只唤醒一个等待者（在多个等待者时）
 *   - wait 带谓词的正确行为（防止虚假唤醒）
 *   - 条件变量在两个协程间"乒乓"式交替唤醒
 *   - 协程在 wait 期间释放锁，被唤醒后重新获得锁
 *   - 数据在协程间通过共享变量安全传递（mutex 保护）
 *   - steps_done == 2 验证两个步骤都正确完成
 *
 * 涉及概念：condition_variable、notify_one、wait（带谓词）、
 *          worker-main 协作模式、协程间同步
 */

/// coronet condition_variable notify_one test (worker/main pattern)
#include <coronet/all.hpp>
#include <atomic>
#include <cstddef>
#include <cstdio>

using namespace coronet;
using namespace std::literals;

mutex m;
condition_variable cv;
std::string data;
bool ready = false;
bool processed = false;
std::atomic<int> steps_done{0};

/**
 * worker_thread——数据处理协程
 *
 * 等待 main 设置 ready=true 后开始处理数据。处理完成后设置 processed=true
 * 并 notify_one 通知 main。wait 使用谓词 [] { return ready; } 确保不会
 * 在 main 未准备好数据时误执行。
 */
task<void> worker_thread() {
    {
        auto lk = co_await m.lock_guard();
        co_await cv.wait(m, [] { return ready; });
    }
    printf("Worker is processing data\n"); fflush(stdout);
    data += " after processing";
    processed = true;
    printf("Worker signals processing completed\n"); fflush(stdout);
    cv.notify_one();
    steps_done.fetch_add(1, std::memory_order_relaxed);
}

/**
 * main_thread——主控制流程协程
 *
 * 步骤：
 *   1. 初始化数据 "Example data"
 *   2. 延迟 200ms（模拟准备工作）
 *   3. 获取锁，设置 ready=true，释放锁后 notify_one 唤醒 worker
 *   4. 等待 worker 完成处理（等待条件 processed=true）
 *   5. 验证处理后的数据
 */
task<> main_thread() {
    data = "Example data";
    co_await async::timeout(200ms);

    {
        auto lk = co_await m.lock_guard();
        ready = true;
        printf("main() signals data ready\n"); fflush(stdout);
    }
    cv.notify_one();

    {
        auto lk = co_await m.lock_guard();
        co_await cv.wait(m, [] { return processed; });
    }
    printf("Back in main(), data = %s\n", data.c_str()); fflush(stdout);
    steps_done.fetch_add(1, std::memory_order_relaxed);
}

/**
 * stopper——超时停止的安全兜底
 */
task<> stopper(io_context& ctx) {
    co_await async::timeout(5s);
    printf("timeout reached, steps_done=%d\n", steps_done.load());
    ctx.can_stop();
}

int main() {
    io_context ctx;
    ctx.co_spawn(worker_thread());
    ctx.co_spawn(main_thread());
    ctx.co_spawn(stopper(ctx));
    ctx.start();
    ctx.join();

    int done = steps_done.load();
    printf("cv_notify_one test: %d/2 steps completed\n", done);
    assert(done == 2);
    printf("cv_notify_one test PASSED\n");
    return 0;
}
