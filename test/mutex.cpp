/**
 * mutex.cpp —— coronet 协程互斥锁正确性测试
 *
 * 测试目的：
 *   验证 coronet 的 mutex 在大量协程并发竞争下的正确性。100 个协程分别在
 *   mutex 保护下对共享计数器进行 10000 次累加，最终计数器应等于
 *   N_COROS * N_INCR = 100 * 10000 = 1000000。任何小于该值的情况都说明
 *   存在数据竞争或锁失效。
 *
 * 测试模式：
 *   - 使用全局 mutex 保护全局 int 计数器 cnt
 *   - 启动 100 个 add() 协程，每个在 lock_guard 保护下执行 10000 次自增
 *   - 使用 monitor 协程轮询 done 原子变量，等待全部 add 协程完成
 *   - monitor 协程使用 async::yield() 让出 CPU，避免忙等待
 *   - 所有 add 完成后 monitor 调用 can_stop() 停止事件循环
 *
 * 关键验证点：
 *   - 协程 mutex 能否正确保护临界区，防止竞态条件
 *   - 100 个协程 × 10000 次自增 = 1000000，结果是否精确
 *   - lock_guard 的 RAII 语义是否正确（锁在作用域结束时自动释放）
 *   - async::yield() 能否让其他协程有机会执行
 *   - 高并发下无死锁、无饥饿
 *
 * 涉及概念：coronet::mutex、lock_guard、async::yield、协程并发
 */

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

/**
 * add——在互斥锁保护下累加计数器的协程
 *
 * 使用 co_await mtx.lock_guard() 获取 RAII 风格的锁保护。
 * 锁定后执行 N_INCR 次自增操作，然后在作用域结束时自动释放锁。
 * done 原子变量记录已完成协程的数量，供 monitor 协程检测。
 */
task<> add() {
    auto lock = co_await mtx.lock_guard();
    for (int i = 0; i < N_INCR; ++i) {
        ++cnt;
    }
    done.fetch_add(1, std::memory_order_relaxed);
}

/**
 * monitor——监控所有 add 协程是否完成的守护协程
 *
 * 使用 async::yield() 让出执行权，同时循环检查 done 是否达到 N_COROS。
 * yield 避免了忙等待（busy waiting），它主动将执行机会让给其他协程。
 * 当所有 add 都完成后，调用 can_stop() 通知 io_context 可以安全退出。
 */
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
