// Tests for channel<T, Capacity> — CSP-style coroutine channel.
//
// 本文件使用 GoogleTest 框架对 coronet::co::channel<T, Capacity> 进行单元测试。
// channel 是通信顺序进程（CSP，Communicating Sequential Processes）风格的
// 协程通道实现，支持协程之间的消息传递。
//
// 模板参数 Capacity（缓冲区容量）决定通道的语义：
//   - Capacity > 0  ：有界缓冲区通道，发送方在缓冲区满时挂起，接收方在缓冲区空时挂起
//   - Capacity == 0 ：汇合式（rendezvous）通道，发送方必须等待接收方同时就绪才能完成传递
//
// 通道是协程安全的消息传递原语，常用于生产者-消费者模型。
//
// 测试覆盖内容：
//   - 有界缓冲区通道的创建和状态检查（empty/full/size）
//   - 单槽位通道的创建
//   - 汇合式通道的创建
//
// 注意：本测试仅验证通道的构造和初始状态，读写操作在 channel_gtest2.cpp 中覆盖。

#include <gtest/gtest.h>
#include "coronet/co/channel.hpp"
#include "coronet/task.hpp"

using namespace coronet;

namespace {

// All channel types are default-constructible

// 验证有界缓冲区通道的构造和初始状态
// Capacity=4 表示通道内部有 4 个槽位的环形缓冲区
// 初始状态应为：空（empty=true）、不满（full=false）、大小为 0
TEST(ChannelTest, CreateBuffered) {
    channel<int, 4> ch;
    EXPECT_TRUE(ch.empty());
    EXPECT_FALSE(ch.full());
    EXPECT_EQ(ch.size(), 0);
}

// 验证单槽位通道的构造
// Capacity=1 是最小有界缓冲区，相当于一个"邮箱"槽位
// 初始状态应为空且未满
TEST(ChannelTest, CreateSingleSlot) {
    channel<int, 1> ch;
    EXPECT_TRUE(ch.empty());
    EXPECT_FALSE(ch.full());
}

// 验证汇合式通道的构造
// Capacity=0 表示没有缓冲区，发送方和接收方必须严格同步：
// 发送操作会挂起直到有接收方就绪，反之亦然
// 初始状态应为空
TEST(ChannelTest, CreateRendezvous) {
    channel<int, 0> ch;
    EXPECT_TRUE(ch.empty());
}

} // namespace
