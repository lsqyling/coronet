// Tests for generator<T> — P2502R2 std::generator reference implementation.
//
// 本文件使用 GoogleTest 框架对 coronet::generator<T> 进行单元测试。
// generator<T> 是 C++23 标准提案 P2502R2 中 std::generator 的参考实现，
// 基于 C++20 协程的 co_yield 语句实现惰性序列生成。
//
// 核心特性：
//   - 惰性求值：序列元素在迭代时才逐个生成
//   - 支持值类型、字符串视图、移动语义等多种返回类型
//   - 通过 co_yield 关键字逐个产出元素
//   - 符合 std::ranges::input_range 概念，支持范围 for 循环
//
// 测试覆盖内容：
//   - 简单的斐波那契数列生成
//   - 字符串视图（std::string_view）序列生成
//   - 仅移动类型（std::unique_ptr）的生成器
//
// 测试模式：直接构造生成器对象，通过 begin()/end() 迭代器遍历所有元素，
// 或使用范围 for 循环收集结果，与预期值进行比对。

#include <gtest/gtest.h>
#include "coronet/generator.hpp"
#include <string>
#include <vector>

using namespace coronet;

namespace {

// 斐波那契数列生成器，上限为 100
// 演示 generator 的基本用法：通过 while 循环配合 co_yield 逐个产出元素
// 每次 co_yield 会挂起生成器，在下一次迭代时恢复继续执行
generator<int> fib() {
    int a = 0, b = 1;
    while (a < 100) {
        co_yield a;
        auto next = a + b;
        a = b;
        b = next;
    }
}

// 验证斐波那契生成器的正确性
// 使用范围 for 循环自动驱动生成器，收集所有元素后与预期序列对比
// 预期序列为前 12 个斐波那契数（0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89）
TEST(GeneratorTest, Fibonacci) {
    std::vector<int> results;
    for (int v : fib()) {
        results.push_back(v);
    }
    EXPECT_EQ(results, (std::vector<int>{0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89}));
}

// 字符串视图序列生成器
// 演示 generator 产出 std::string_view 类型（轻量级字符串引用，不涉及拷贝）
generator<std::string_view> names() {
    co_yield "alice";
    co_yield "bob";
    co_yield "charlie";
}

// 验证字符串视图生成器的迭代器行为
// 手动管理迭代器：获取 begin() 迭代器后逐个解引用和递增
// 注意：迭代器在解引用时返回的字符串视图的生命周期由生成器协程的承诺对象管理
// 最后一个元素消耗完后，迭代器应等于 std::default_sentinel（哨兵迭代器）
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
// 仅移动类型的生成器示例，使用 std::unique_ptr<int>
// 验证 generator 可以正确处理不可复制（仅可移动）的元素类型
// co_yield 时通过 std::make_unique 创建新对象，所有权转移给消费者
generator<std::unique_ptr<int>> ptrs() {
    co_yield std::make_unique<int>(1);
    co_yield std::make_unique<int>(2);
}

// 验证仅移动类型生成器的使用
// 范围 for 循环中 auto&& 捕获右值引用，避免不必要的拷贝
// 遍历所有元素后累加指针所指向的值
TEST(GeneratorTest, MoveOnly) {
    int sum = 0;
    for (auto&& p : ptrs()) {
        sum += *p;
    }
    EXPECT_EQ(sum, 3);
}

} // namespace
