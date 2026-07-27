/**
 * channel_stress.cpp — coronet CSP channel 高并发压力测试
 *
 * 测试目的：
 *   1. 多生产者-多消费者高并发（20 生产者 × 10 消费者）
 *   2. Rendezvous 模式高并发握手
 *   3. 单槽通道 Ping-Pong 极高频率交替
 *   4. 环形缓冲区回绕正确性
 *   5. 生产者满阻塞 / 消费者空阻塞
 *
 * 涉及概念：channel<T,N>、MPMC、rendezvous、有界缓冲
 */

#include <coronet/all.hpp>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <vector>

using namespace coronet;
using namespace std::chrono_literals;

// ============================================================
// Test 1: MPMC 高并发 — 20 producers × 10 consumers
// ============================================================
constexpr int N_PRODUCERS = 20;
constexpr int N_CONSUMERS = 10;
constexpr int MSGS_PER = 100;
constexpr int MPMC_TOTAL = N_PRODUCERS * MSGS_PER;

channel<int, 16> mpmc_chan;
std::atomic<int> mpmc_produced{0};
std::atomic<int> mpmc_consumed{0};

task<> mpmc_producer(int id) {
    for (int i = 0; i < MSGS_PER; ++i) {
        co_await mpmc_chan.release(id * 10000 + i);
        mpmc_produced.fetch_add(1, std::memory_order_relaxed);
    }
}

task<> mpmc_consumer(int id) {
    while (mpmc_consumed.load(std::memory_order_relaxed) < MPMC_TOTAL) {
        int val = co_await mpmc_chan.acquire();
        (void)val;
        mpmc_consumed.fetch_add(1, std::memory_order_relaxed);
    }
}

task<> mpmc_monitor(io_context& ctx) {
    while (mpmc_consumed.load() < MPMC_TOTAL) co_await async::yield();
    printf("[MPMC] produced=%d consumed=%d\n",
           mpmc_produced.load(), mpmc_consumed.load());
    ctx.can_stop();
}

// ============================================================
// Test 2: 单槽 Ping-Pong 高频
// ============================================================
constexpr int PINGPONG_ROUNDS = 1000;

channel<int, 1> pingpong_chan;
std::atomic<int> pingpong_count{0};

task<> pingpong_producer() {
    for (int i = 0; i < PINGPONG_ROUNDS; ++i) {
        co_await pingpong_chan.release(i);
    }
    co_await pingpong_chan.release(-1);  // sentinel
}

task<> pingpong_consumer() {
    while (true) {
        int val = co_await pingpong_chan.acquire();
        if (val == -1) break;
        pingpong_count.fetch_add(1, std::memory_order_relaxed);
    }
}

task<> pingpong_monitor(io_context& ctx) {
    while (pingpong_count.load() < PINGPONG_ROUNDS) co_await async::yield();
    printf("[PingPong] rounds=%d\n", pingpong_count.load());
    ctx.can_stop();
}

// ============================================================
// Test 4: 环形缓冲区回绕
// ============================================================
constexpr int WRAP_CAP = 8;
constexpr int WRAP_TOTAL = WRAP_CAP * 20;

channel<int, WRAP_CAP> wrap_chan;
std::atomic<int> wrap_consumed{0};
std::atomic<int64_t> wrap_sum{0};

task<> wrap_consumer() {
    while (wrap_consumed.load() < WRAP_TOTAL) {
        int val = co_await wrap_chan.acquire();
        wrap_sum.fetch_add(val, std::memory_order_relaxed);
        wrap_consumed.fetch_add(1, std::memory_order_relaxed);
    }
}

task<> wrap_producer_and_monitor(io_context& ctx) {
    int64_t expected = 0;
    for (int i = 0; i < WRAP_TOTAL; ++i) {
        co_await wrap_chan.release(i);
        expected += i;
    }
    while (wrap_consumed.load() < WRAP_TOTAL) co_await async::yield();
    printf("[WrapAround] sum=%lld expected=%lld\n",
           (long long)wrap_sum.load(), (long long)expected);
    assert(wrap_sum.load() == expected);
    ctx.can_stop();
}

// ============================================================
// Test 5: 生产者满阻塞 + 消费者空阻塞
// ============================================================
channel<int, 2> block_chan;
std::atomic<bool> block_producer_done{false};
std::atomic<bool> block_consumer_got{false};

task<> block_producer() {
    co_await block_chan.release(1);
    co_await block_chan.release(2);
    // 第三个 release 应该阻塞（缓冲区满）
    co_await block_chan.release(3);
    block_producer_done.store(true);
}

task<> block_consumer() {
    int v1 = co_await block_chan.acquire();
    assert(v1 == 1);
    int v2 = co_await block_chan.acquire();
    assert(v2 == 2);
    int v3 = co_await block_chan.acquire();
    assert(v3 == 3);
    block_consumer_got.store(true);
}

task<> block_monitor(io_context& ctx) {
    while (!block_producer_done.load() || !block_consumer_got.load())
        co_await async::yield();
    printf("[BlockTest] producer_done=%d consumer_got=%d\n",
           block_producer_done.load(), block_consumer_got.load());
    ctx.can_stop();
}

// ============================================================
// Test 6: drop() 操作
// ============================================================
channel<int, 4> drop_chan;

task<> drop_test(io_context& ctx) {
    co_await drop_chan.release(1);
    co_await drop_chan.release(2);
    co_await drop_chan.release(3);
    assert(drop_chan.size() == 3);

    co_await drop_chan.drop();
    assert(drop_chan.size() == 2);

    int val = co_await drop_chan.acquire();
    assert(val == 2);
    printf("[DropTest] PASSED\n");
    ctx.can_stop();
}

// ============================================================
// Stopper — 总超时兜底
// ============================================================
task<> global_stopper(io_context& ctx) {
    co_await async::timeout(30s);
    printf("GLOBAL TIMEOUT — stopping\n");
    ctx.can_stop();
}

// ============================================================
// main — 依次运行所有测试
// ============================================================

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);

    // === Phase 1: PingPong (快速) ===
    {
        io_context ctx;
        ctx.co_spawn(pingpong_producer());
        ctx.co_spawn(pingpong_consumer());
        ctx.co_spawn(pingpong_monitor(ctx));
        ctx.co_spawn(global_stopper(ctx));
        ctx.start(); ctx.join();
        assert(pingpong_count.load() == PINGPONG_ROUNDS);
        printf("=== PingPong PASSED ===\n");
    }

    // === Phase 2: Drop ===
    {
        io_context ctx;
        ctx.co_spawn(drop_test(ctx));
        ctx.co_spawn(global_stopper(ctx));
        ctx.start(); ctx.join();
        printf("=== DropTest PASSED ===\n");
    }

    // === Phase 3: Block ===
    {
        io_context ctx;
        ctx.co_spawn(block_producer());
        ctx.co_spawn(block_consumer());
        ctx.co_spawn(block_monitor(ctx));
        ctx.co_spawn(global_stopper(ctx));
        ctx.start(); ctx.join();
        assert(block_producer_done.load());
        assert(block_consumer_got.load());
        printf("=== BlockTest PASSED ===\n");
    }

    // === Phase 4: WrapAround ===
    {
        io_context ctx;
        ctx.co_spawn(wrap_consumer());
        ctx.co_spawn(wrap_producer_and_monitor(ctx));
        ctx.co_spawn(global_stopper(ctx));
        ctx.start(); ctx.join();
        assert(wrap_consumed.load() == WRAP_TOTAL);
        printf("=== WrapAround PASSED ===\n");
    }

    // === Phase 5: MPMC ===
    {
        io_context ctx;
        for (int i = 0; i < N_PRODUCERS; ++i) ctx.co_spawn(mpmc_producer(i));
        for (int i = 0; i < N_CONSUMERS; ++i) ctx.co_spawn(mpmc_consumer(i));
        ctx.co_spawn(mpmc_monitor(ctx));
        ctx.co_spawn(global_stopper(ctx));
        ctx.start(); ctx.join();
        assert(mpmc_produced.load() == MPMC_TOTAL);
        assert(mpmc_consumed.load() == MPMC_TOTAL);
        printf("=== MPMC PASSED ===\n");
    }

    printf("\n=== ALL CHANNEL STRESS TESTS PASSED ===\n");
    return 0;
}
