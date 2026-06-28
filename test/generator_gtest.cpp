// Tests for generator<T> — P2502R2 std::generator reference implementation.
#include <gtest/gtest.h>
#include "coronet/generator.hpp"
#include <string>
#include <vector>

using namespace coronet;

namespace {

generator<int> fib() {
    int a = 0, b = 1;
    while (a < 100) {
        co_yield a;
        auto next = a + b;
        a = b;
        b = next;
    }
}

TEST(GeneratorTest, Fibonacci) {
    std::vector<int> results;
    for (int v : fib()) {
        results.push_back(v);
    }
    EXPECT_EQ(results, (std::vector<int>{0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89}));
}

generator<std::string_view> names() {
    co_yield "alice";
    co_yield "bob";
    co_yield "charlie";
}

TEST(GeneratorTest, StringViews) {
    auto g = names();
    auto it = g.begin();
    EXPECT_EQ(*it, "alice");
    ++it;
    EXPECT_EQ(*it, "bob");
    ++it;
    EXPECT_EQ(*it, "charlie");
    ++it;
    EXPECT_EQ(it, std::default_sentinel);
}

// Move-only value type
generator<std::unique_ptr<int>> ptrs() {
    co_yield std::make_unique<int>(1);
    co_yield std::make_unique<int>(2);
}

TEST(GeneratorTest, MoveOnly) {
    int sum = 0;
    for (auto&& p : ptrs()) {
        sum += *p;
    }
    EXPECT_EQ(sum, 3);
}

} // namespace
