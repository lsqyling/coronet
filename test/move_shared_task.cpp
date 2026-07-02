/// Test coronet::shared_task multi-await semantics.
/// Ported from co_context/test/move_shared_task.cpp
///
/// 本文件测试 coronet::shared_task 的多等待（multi-await）语义和移动语义。
/// shared_task 是引用计数共享所有权的协程类型，与 task<T> 的关键区别在于：
///
///   task<T>           |   shared_task<T>
///   ------------------|--------------------------------
///   唯一所有权        |   引用计数共享所有权
///   不可复制          |   可复制（浅拷贝，共享底层协程）
///   单等待者          |   多等待者
///                     |   每个等待者获得相同的结果
///
/// 测试覆盖以下场景：
///   1. 多次 co_await 同一 shared_task，验证返回相同的值
///   2. co_await 后通过 std::move 获取返回值（转移所有权）
///   3. 在移动获取值后再次 co_await，验证内部值的生命周期管理
///   4. 复制 shared_task 并通过副本等待
///   5. 整数类型的 shared_task，验证非字符串类型的行为

#include <coronet/shared_task.hpp>
#include <coronet/io_context.hpp>

#include <cassert>
#include <cstdio>
#include <string>
#include <utility>

using namespace coronet;

// 创建返回 std::string 的 shared_task 协程
// 返回固定字符串 "shared_task_value"
shared_task<std::string> make_shared_str() {
    co_return "shared_task_value";
}

// 创建返回 int 的 shared_task 协程
shared_task<int> make_shared_int() {
    co_return 42;
}

task<> run() {
    std::printf("=== Test: shared_task ===\n");

    // Test 1: multiple co_await on same shared_task returns same value
    //
    // 验证同一个 shared_task 的多次 co_await 返回相同的值
    // 这是 shared_task 的核心语义：协程完成后，所有后续等待者
    // 都能立即获取已经存储的结果，无需重新执行协程
    auto t_str = make_shared_str();
    std::string s1 = co_await t_str;
    std::printf("  await 1: '%s'\n", s1.c_str());
    assert(s1 == "shared_task_value");
    std::printf("  PASS\n");

    std::string s2 = co_await t_str;
    std::printf("  await 2: '%s'\n", s2.c_str());
    assert(s2 == "shared_task_value");
    std::printf("  PASS\n");

    // Test 2: move from co_await result
    //
    // 从 co_await 表达式中移动结果
    // co_await 返回的是 promise 中存储的值的引用，
    // 通过 std::move 可以转移该值的所有权（对于 std::string 是移动而非拷贝）
    std::string s3 = co_await std::move(t_str);
    std::printf("  moved: '%s'\n", s3.c_str());
    assert(s3 == "shared_task_value");
    std::printf("  PASS\n");

    // Test 3: await again after move (still valid, returns stored value)
    //
    // 在通过 std::move(co_await t_str) 获取值后，再次 co_await 同一个 shared_task
    // 注意：此时 promise 中的值已经被移动（空字符串），
    // 再次等待会返回移动后的空值状态
    // 这个测试验证了 shared_task 的内部值在移动后的行为
    std::string s4 = co_await t_str;
    std::printf("  await after move: '%s'\n", s4.c_str());
    assert(s4 == "shared_task_value");
    std::printf("  PASS\n");

    // Test 4: move result into variable
    //
    // 使用 std::move 显式移动 co_await 的结果到局部变量
    // 这与 Test 2 类似，但使用不同的语法形式
    // 注意：这会将 promise 内部的值移出，之后 promise 中的值变为空
    std::string s5 = std::move(co_await t_str);
    std::printf("  std::moved result: '%s'\n", s5.c_str());
    assert(s5 == "shared_task_value");
    std::printf("  PASS\n");

    // Test 5: copy semantics (note: value was moved in test 4 step 5,
    // so the promise's value is now empty)
    //
    // 验证 shared_task 的复制语义：
    //   1. shared_task 支持复制（拷贝构造函数），这是与 task<T> 的关键区别
    //   2. 复制后 t2 与 t_str 共享同一个协程状态（引用计数增加）
    //   3. 由于上一步 Test 4 中通过 std::move 移出了 promise 中的值，
    //      此时 promise 内部的值处于"移动后"状态（空字符串）
    //   4. 通过 t2 等待获取的是移动后的空值
    // 这个测试揭示了 shared_task 的一个重要特性：
    // 如果某个等待者移走了 promise 中的值，后续等待者将看到空值
    auto t2 = t_str;  // shared_task is copyable
    std::string s6 = co_await t2;
    // After std::move(co_await t) above, the internal value is moved-from
    assert(s6.empty());
    std::printf("  PASS\n");

    // Test 6: int value type
    //
    // 验证整数类型的 shared_task
    // 整数这种平凡可复制的类型不受移动语义影响
    // 多次 co_await 都返回正确的整数值 42
    auto t_int = make_shared_int();
    int v1 = co_await t_int;
    assert(v1 == 42);
    int v2 = co_await t_int;
    assert(v2 == 42);
    std::printf("  PASS\n");

    std::printf("  All shared_task tests passed!\n");
}

int main() {
    // 标准测试模式：在 io_context 事件循环中执行协程
    io_context ctx;
    ctx.co_spawn(run());
    ctx.start();
    ctx.can_stop();
    ctx.join();
    return 0;
}
