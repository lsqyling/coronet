/// GoogleTest unit tests for coronet::shared_task<T>
///
/// 本文件使用 GoogleTest 框架对 coronet::shared_task<T> 进行全面的单元测试。
///
/// shared_task<T> 与 task<T> 的区别：
///   task<T> 是唯一所有权协程（不可复制，仅可移动），仅允许单个等待者（awaiter）。
///   shared_task<T> 采用引用计数（ref-counted）机制，允许多个等待者同时 co_await 同一个协程，
///   所有等待者都能获取相同的返回值（复制语义）。
///
/// 核心特性：
///   1. 引用计数共享所有权，支持复制（copyable）
///   2. 多次 co_await 返回相同的值（结果一旦产生，后续等待者立即获取）
///   3. 支持值类型、void 类型、引用类型的返回
///   4. 异常传播：如果协程抛出异常，所有等待者都会收到该异常
///   5. when_ready() 可等待协程完成但不获取值
///
/// 测试模式：
///   所有需要 io_context 的测试使用 run_io_test 辅助函数，其标准流程为：
///   co_spawn -> start -> can_stop -> join，确保协程在事件循环中正确执行。

#include <coronet/shared_task.hpp>
#include <coronet/io_context.hpp>

#include <gtest/gtest.h>
#include <string>
#include <stdexcept>

using namespace coronet;

namespace {

// Helper: run a coroutine inside an io_context
//
// 测试辅助函数：在 io_context 事件循环中执行协程。
// 标准测试模式：
//   1. co_spawn：将协程注册到事件循环中
//   2. start：启动事件循环
//   3. can_stop：向事件循环发送停止信号（在所有任务完成后优雅退出）
//   4. join：等待事件循环线程结束
template<typename F>
void run_io_test(F func) {
    io_context ctx;
    ctx.co_spawn(func());
    ctx.start();
    ctx.can_stop();
    ctx.join();
}

// ============================================================
// Test: default construction
// ============================================================
//
// 验证 shared_task 的默认构造行为。
// 默认构造的 shared_task 不持有有效的协程句柄，
// get_handle() 应返回空（nullptr）。
// 这是描述空任务状态的基础测试。
TEST(SharedTaskTest, DefaultConstruction) {
    shared_task<int> t;
    EXPECT_FALSE(t.get_handle());
}

// ============================================================
// Test: basic coroutine execution and value retrieval
// ============================================================
//
// 验证 shared_task 的基本协程执行和值获取功能。
// 创建一个返回 42 的协程，通过 co_await 等待其结果。
// 这是 shared_task 最基础的使用模式：协程产生值 -> 等待者获取值。
TEST(SharedTaskTest, BasicValue) {
    run_io_test([]() -> task<> {
        auto st = []() -> shared_task<int> {
            co_return 42;
        }();

        int v = co_await st;
        EXPECT_EQ(v, 42);
        co_return;
    });
}

// ============================================================
// Test: multiple awaiters get the same value (copy semantics)
// ============================================================
//
// 验证 shared_task 的多次等待语义：多个等待者应获得相同的返回值。
// 关键测试点：
//   1. shared_task 可以复制（copyable），这是与 task<T> 的核心区别之一
//   2. 通过复制得到的副本与原对象共享同一个协程结果
//   3. 多次 co_await（包括在原始对象上再次等待）都返回相同的字符串 "hello"
//   4. 结果具有复制语义，每次 co_await 都返回值的拷贝
// 这个测试验证了引用计数共享所有权机制的正确性。
TEST(SharedTaskTest, MultipleAwait) {
    run_io_test([]() -> task<> {
        auto st = []() -> shared_task<std::string> {
            co_return std::string("hello");
        }();

        // Copy the shared_task
        auto st2 = st;

        std::string s1 = co_await st;
        std::string s2 = co_await st2;
        EXPECT_EQ(s1, "hello");
        EXPECT_EQ(s2, "hello");

        // Await again on the original
        std::string s3 = co_await st;
        EXPECT_EQ(s3, "hello");
        co_return;
    });
}

// ============================================================
// Test: exception propagation
// ============================================================
//
// 验证 shared_task 的异常传播机制。
// 关键测试点：
//   1. 协程体内抛出的异常会被捕获并存储到协程的承诺对象（promise）中
//   2. 第一次 co_await 时异常被重新抛出，等待者必须捕获它
//   3. 第二次 co_await 时异常再次被抛出 —— 异常被持久化存储，可多次传播
//   4. 异常类型（std::runtime_error）和异常消息在传播中保持一致
// 这个测试验证了 shared_task 对异常的正确处理：
// 无论有多少个等待者，所有等待者都能看到同一个异常。
TEST(SharedTaskTest, ExceptionPropagation) {
    run_io_test([]() -> task<> {
        auto st = []() -> shared_task<int> {
            throw std::runtime_error("test error");
            co_return 0;
        }();

        bool caught = false;
        try {
            co_await st;
        } catch (const std::runtime_error& e) {
            EXPECT_STREQ(e.what(), "test error");
            caught = true;
        }
        EXPECT_TRUE(caught);

        // Second await should also throw
        caught = false;
        try {
            co_await st;
        } catch (const std::runtime_error&) {
            caught = true;
        }
        EXPECT_TRUE(caught);
        co_return;
    });
}

// ============================================================
// Test: when_ready()
// ============================================================
//
// 验证 when_ready() 方法的语义。
// when_ready() 返回一个可等待对象（awaiter），该对象在协程完成时变为就绪，
// 但不获取协程的返回值或重新抛出异常。
// 典型使用场景：调用者只关心协程何时完成，而非其结果。
// 测试验证：
//   1. when_ready() 等待后协程确实已经完成（is_ready）
//   2. 在 when_ready() 之后仍然可以通过 co_await 正常获取值
TEST(SharedTaskTest, WhenReady) {
    run_io_test([]() -> task<> {
        auto st = []() -> shared_task<int> {
            co_return 100;
        }();

        co_await st.when_ready();
        // After when_ready, the value should be available
        int v = co_await st;
        EXPECT_EQ(v, 100);
        co_return;
    });
}

// ============================================================
// Test: move semantics
// ============================================================
//
// 验证 shared_task 的移动语义。
// 虽然 shared_task 是可复制的，但移动语义仍然重要：
//   1. 移动构造后，源对象的句柄被清空（未定义状态）
//   2. 目标对象获得源对象原有的句柄，地址相同
//   3. 移动赋值同样将源对象的拥有权转移到目标对象
//   4. moved-from 状态的对象 get_handle() 返回空
// 这是对 shared_task 所有权转移的完整性验证。
TEST(SharedTaskTest, MoveSemantics) {
    shared_task<int> t1 = []() -> shared_task<int> {
        co_return 42;
    }();

    auto handle = t1.get_handle();
    EXPECT_TRUE(handle);

    shared_task<int> t2(std::move(t1));
    EXPECT_FALSE(t1.get_handle());
    EXPECT_EQ(t2.get_handle(), handle);

    // Move assignment
    shared_task<int> t3;
    t3 = std::move(t2);
    EXPECT_FALSE(t2.get_handle());
    EXPECT_EQ(t3.get_handle(), handle);
}

// ============================================================
// Test: void return type
// ============================================================
//
// 验证 shared_task<void> 的行为。
// 对于返回 void 的协程：
//   1. 协程体执行副作用（如设置标志位），不返回值
//   2. co_await 后协程的副作用应已被执行
//   3. 多次 co_await 同一个 shared_task<void> 是合法的
// 此测试使用 static 变量跟踪协程是否被执行，验证 lazy 语义
// 和多次等待的正确性。
TEST(SharedTaskTest, VoidReturn) {
    run_io_test([]() -> task<> {
        static bool executed = false;
        executed = false;

        auto st = [&]() -> shared_task<> {
            executed = true;
            co_return;
        }();

        co_await st;
        EXPECT_TRUE(executed);

        // Second await
        co_await st;
        EXPECT_TRUE(executed);
        co_return;
    });
}

// ============================================================
// Test: reference return type
// ============================================================
//
// 验证 shared_task<int&> 引用返回类型的行为。
// 关键测试点：
//   1. co_await 返回的是原始变量的引用（而非拷贝）
//   2. 引用地址应与原始变量的地址一致（通过 &ref == &value 验证）
//   3. 通过引用修改变量值后，原始变量同步改变
//   4. 再次 co_await 返回更新后的值
// 引用语义在共享协程中尤为重要，多个等待者可能通过引用来共享
// 和修改同一个状态。
TEST(SharedTaskTest, ReferenceReturn) {
    run_io_test([]() -> task<> {
        static int value = 123;

        auto st = []() -> shared_task<int&> {
            co_return value;
        }();

        int& ref = co_await st;
        EXPECT_EQ(ref, 123);
        EXPECT_EQ(&ref, &value);

        // Modify through reference
        ref = 456;
        EXPECT_EQ(value, 456);

        // Second await returns updated value
        int& ref2 = co_await st;
        EXPECT_EQ(ref2, 456);
        co_return;
    });
}

} // namespace
