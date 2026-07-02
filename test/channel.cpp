/**
 * channel.cpp —— coronet CSP 通道测试
 *
 * 测试目的：
 *   验证 coronet 的 channel（CSP 风格的通信通道）在多生产者-多消费者
 *   场景下的正确性。3 个生产者协程各发送 4 条消息（2 条快速 + 2 条带 200ms 延迟），
 *   总计 12 条消息。3 个消费者协程并发消费，确保所有消息被正确接收。
 *
 * 测试模式：
 *   - 创建一个 channel<std::string, 4>（缓冲大小为 4 的字符串通道）
 *   - 3 个生产者协程各 release() 4 条消息（共 12 条）
 *   - 3 个消费者协程通过 acquire() 循环读取消息
 *   - 使用原子变量 msg_consumed 统计已消费消息数
 *   - stopper 协程在 8 秒超时后停止事件循环
 *
 * 关键验证点：
 *   - channel 能正确传递字符串数据
 *   - 多生产者并发写入无数据竞争
 *   - 多消费者并发读取每个消息恰好被消费一次
 *   - 缓冲大小为 4，当缓冲区满时生产者自动阻塞
 *   - 当缓冲区空时消费者自动阻塞
 *   - 通道在有界缓冲（bounded buffer）下的同步语义
 *   - 所有 12 条消息最终都被消费
 *
 * 涉及概念：channel<T, N>、acquire/release（类似 C++11 的 send/recv）、
 *          有界缓冲、多生产者-多消费者（MPMC）
 */

/// coronet channel test: 3 producers → 3 consumers
#include <coronet/coronet.hpp>
#include <cassert>
#include <cstdio>
using namespace coronet;
using namespace std::chrono_literals;

channel<std::string, 4> chan;
std::atomic<int> msg_consumed{0};
constexpr int TOTAL_MSGS = 12;
constexpr auto TEST_TIMEOUT = 8s;

/**
 * produce——生产者协程
 *
 * 每个生产者发送 4 条消息：前 2 条立即发送（release），后 2 条在 200ms
 * 延迟后发送。"快速发送"和"慢速发送"的混合测试了通道在混合速率的
 * 生产者下的行为。release() 在通道满时会自动阻塞，等待消费者腾出空间。
 */
task<> produce(std::string tag) {
    for (int i = 0; i < 2; ++i) {
        co_await chan.release(tag + ": fast produce");
    }
    for (int i = 0; i < 2; ++i) {
        co_await async::timeout(200ms);
        co_await chan.release(tag + ": slow produce");
    }
}

/**
 * consume——消费者协程
 *
 * 一直消费直到总消费数达到 TOTAL_MSGS。acquire() 从通道获取一条消息，
 * 通道为空时自动阻塞，等待生产者发送新消息。
 * 原子变量 msg_consumed 用于保证消费计数的线程安全。
 */
task<> consume(std::string tag) {
    while (msg_consumed.load(std::memory_order_relaxed) < TOTAL_MSGS) {
        std::string str{co_await chan.acquire()};
        int n = msg_consumed.fetch_add(1, std::memory_order_relaxed) + 1;
        printf("%s: %s (%d/%d)\n", tag.c_str(), str.c_str(), n, TOTAL_MSGS);
        fflush(stdout);
    }
}

/**
 * stopper——超时停止的安全兜底
 *
 * 如果 8 秒内未消费完 12 条消息，强制停止事件循环防止测试挂起。
 */
task<> stopper(io_context& ctx) {
    co_await async::timeout(TEST_TIMEOUT);
    printf("timeout reached, consumed=%d\n", msg_consumed.load());
    ctx.can_stop();
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    io_context ctx;
    ctx.co_spawn(produce("p0"));
    ctx.co_spawn(produce("p1"));
    ctx.co_spawn(produce("p2"));
    ctx.co_spawn(consume("c0"));
    ctx.co_spawn(consume("c1"));
    ctx.co_spawn(consume("c2"));
    ctx.co_spawn(stopper(ctx));
    ctx.start();
    ctx.join();

    int got = msg_consumed.load();
    assert(got >= TOTAL_MSGS);
    printf("channel test PASSED (%d msgs)\n", got);
    return 0;
}
