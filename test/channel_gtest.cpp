// Tests for channel<T, Capacity> — CSP-style coroutine channel.
#include <gtest/gtest.h>
#include "coronet/co/channel.hpp"
#include "coronet/task.hpp"

using namespace coronet;

namespace {

// All channel types are default-constructible
TEST(ChannelTest, CreateBuffered) {
    channel<int, 4> ch;
    EXPECT_TRUE(ch.empty());
    EXPECT_FALSE(ch.full());
    EXPECT_EQ(ch.size(), 0);
}

TEST(ChannelTest, CreateSingleSlot) {
    channel<int, 1> ch;
    EXPECT_TRUE(ch.empty());
    EXPECT_FALSE(ch.full());
}

TEST(ChannelTest, CreateRendezvous) {
    channel<int, 0> ch;
    EXPECT_TRUE(ch.empty());
}

} // namespace
