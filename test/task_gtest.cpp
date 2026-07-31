// Tests for task<T> — lazy coroutine with inline parent-chain execution.
//
// 本文件使用 GoogleTest 框架对 coronet::task<T> 进行单元测试。
// task<T> 是 C++20 协程的返回类型，采用惰性求值策略：
// 协程体只有在被 co_await 时才开始执行，而非在构造时立即启动。
// 其核心设计特征包括：
//   1. 唯一所有权语义（不可复制，仅可移动）
//   2. 父子链式执行（父协程在 co_await 子协程时作为内联调用链执行）
//   3. 支持值类型 / void / 引用类型的协程返回
//
// 测试覆盖内容：
//   - 各种返回类型的 task 创建（值、void、引用）
//   - 惰性语义验证（协程创建后不会立即执行）
//   - 移动语义（moved-from 状态检查）
//   - 实际 co_await 并验证返回值
//   - 异常通过 co_await 传播
//   - when_ready() 不抛异常
//   - detach 操作（仅 task<void>）
//   - task<T&> 引用返回验证

#include <gtest/gtest.h>
#include "coronet/task.hpp"

#include <stdexcept>
#include <string>
#include <utility>

using namespace coronet;

namespace {

// ============================================================
// Coroutine helper functions
// ============================================================

task<int> make_value(int v) { co_return v; }
task<std::string> make_string(const char* s) { co_return std::string(s); }
task<void> make_void() { co_return; }
task<int&> make_ref(int& r) { co_return r; }

task<int> throw_int(const char* msg) {
    throw std::runtime_error(msg);
    co_return 0;
}

task<void> throw_void(const char* msg) {
    throw std::logic_error(msg);
}

// ============================================================
// Runner: manually resume a root task to completion.
//
// task uses inline parent-chain execution — when a parent co_awaits
// a child, the child runs synchronously on the same stack. So we
// can drive a root task by calling resume() once; all nested
// co_awaits execute inline.
// ============================================================

template<typename T>
T run_task(task<T> t) {
    auto h = t.get_handle();
    assert(h);
    h.resume();
    // h is now suspended at final_suspend, done() == true
    if constexpr (std::is_void_v<T>) {
        h.promise().result();  // may rethrow stored exception
    } else {
        // Copy result before t's destructor destroys the frame
        T result = h.promise().result();
        return result;
    }
}

// Specialization for task<T&> — returns a reference, no copy needed
template<typename T>
T& run_task_ref(task<T&> t) {
    auto h = t.get_handle();
    assert(h);
    h.resume();
    return h.promise().result();
}

// ============================================================
// 1. Basic creation and lazy semantics
// ============================================================

TEST(TaskTest, CreateValue) {
    auto t = make_value(42);
    EXPECT_FALSE(t.is_ready());  // lazy: not started yet
}

TEST(TaskTest, CreateVoid) {
    auto t = make_void();
    EXPECT_FALSE(t.is_ready());
}

TEST(TaskTest, CreateString) {
    auto t = make_string("hello");
    EXPECT_FALSE(t.is_ready());
}

TEST(TaskTest, CreateRef) {
    int x = 10;
    auto t = make_ref(x);
    EXPECT_FALSE(t.is_ready());
}

// ============================================================
// 2. Move semantics
// ============================================================

TEST(TaskTest, MoveConstruct) {
    auto t1 = make_value(1);
    auto t2 = std::move(t1);
    EXPECT_TRUE(t1.is_ready());   // moved-from is empty (ready)
    EXPECT_FALSE(t2.is_ready());  // new owner is still pending
}

TEST(TaskTest, MoveAssign) {
    auto t1 = make_value(1);
    task<int> t2;
    t2 = std::move(t1);
    EXPECT_TRUE(t1.is_ready());
    EXPECT_FALSE(t2.is_ready());
}

TEST(TaskTest, SelfMoveAssign) {
    auto t = make_value(42);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
    t = std::move(t);  // intentional self-move, should be no-op
#pragma GCC diagnostic pop
    EXPECT_FALSE(t.is_ready());  // still valid
}

// ============================================================
// 3. Actual co_await with result verification
// ============================================================

TEST(TaskTest, AwaitValue) {
    int result = run_task(make_value(42));
    EXPECT_EQ(result, 42);
}

TEST(TaskTest, AwaitVoid) {
    EXPECT_NO_THROW(run_task(make_void()));
}

TEST(TaskTest, AwaitString) {
    std::string result = run_task(make_string("Hello, Task!"));
    EXPECT_EQ(result, "Hello, Task!");
}

TEST(TaskTest, AwaitMultiple) {
    // Run multiple tasks sequentially
    int r1 = run_task(make_value(10));
    int r2 = run_task(make_value(20));
    int r3 = run_task(make_value(30));
    EXPECT_EQ(r1 + r2 + r3, 60);
}

// ============================================================
// 4. Chained co_await (nested coroutines)
// ============================================================

task<int> inner_value() { co_return 10; }

task<int> chained_value() {
    auto inner = inner_value();
    co_return co_await inner + 5;
}

TEST(TaskTest, ChainedAwait) {
    int result = run_task(chained_value());
    EXPECT_EQ(result, 15);
}

// ============================================================
// 5. Exception propagation through co_await
// ============================================================

TEST(TaskTest, ExceptionInt) {
    try {
        run_task(throw_int("test error"));
        FAIL() << "Expected runtime_error";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "test error");
    }
}

TEST(TaskTest, ExceptionVoid) {
    try {
        run_task(throw_void("void error"));
        FAIL() << "Expected logic_error";
    } catch (const std::logic_error& e) {
        EXPECT_STREQ(e.what(), "void error");
    }
}

// ============================================================
// 6. task<T&> reference return
// ============================================================

TEST(TaskTest, ReferenceReturn) {
    int value = 42;
    int& ref = run_task_ref(make_ref(value));
    EXPECT_EQ(ref, 42);
    EXPECT_EQ(&ref, &value);  // same address

    // Modify through reference
    ref = 100;
    EXPECT_EQ(value, 100);
}

// ============================================================
// 7. when_ready()
// ============================================================

task<void> coro_when_ready() {
    task<int> t = make_value(100);
    co_await t.when_ready();
    // After when_ready, task is done but we didn't get the result
    assert(t.is_ready());
    co_return;
}

TEST(TaskTest, WhenReady) {
    EXPECT_NO_THROW(run_task(coro_when_ready()));
}

// ============================================================
// 8. is_ready() after co_await
// ============================================================

task<void> coro_is_ready_after_await() {
    task<int> t = make_value(42);
    EXPECT_FALSE(t.is_ready());
    co_await t;
    EXPECT_TRUE(t.is_ready());
    co_return;
}

TEST(TaskTest, IsReadyAfterAwait) {
    EXPECT_NO_THROW(run_task(coro_is_ready_after_await()));
}

// ============================================================
// 9. detach() — only for task<void>
// ============================================================

TEST(TaskTest, DetachVoid) {
    bool executed = false;
    auto set_flag = [&executed]() -> task<void> {
        executed = true;
        co_return;
    };

    task<void> t = set_flag();
    auto h = t.get_handle();
    t.detach();
    EXPECT_FALSE(t.get_handle());  // handle cleared

    // Manually resume the detached handle.
    // After detach(), final_suspend's await_ready() returns true,
    // so the runtime automatically destroys the frame when the
    // coroutine completes. Do NOT call h.destroy() afterwards.
    h.resume();
    EXPECT_TRUE(executed);
}

// ============================================================
// 10. co_await rvalue (move semantics in await)
// ============================================================

task<void> coro_await_rvalue() {
    [[maybe_unused]] int result = co_await make_value(99);
    assert(result == 99);

    std::string s = co_await make_string("rvalue");
    assert(s == "rvalue");
    co_return;
}

TEST(TaskTest, AwaitRvalue) {
    EXPECT_NO_THROW(run_task(coro_await_rvalue()));
}

// ============================================================
// 11. swap()
// ============================================================

task<void> coro_swap() {
    task<int> t1 = make_value(10);
    task<int> t2 = make_value(20);
    swap(t1, t2);
    [[maybe_unused]] int r1 = co_await t1;
    [[maybe_unused]] int r2 = co_await t2;
    assert(r1 == 20 && r2 == 10);
    co_return;
}

TEST(TaskTest, Swap) {
    EXPECT_NO_THROW(run_task(coro_swap()));
}

// ============================================================
// 12. Exception in nested coroutine propagates to root
// ============================================================

task<int> nested_throw() {
    co_await throw_void("nested");
    co_return 0;
}

TEST(TaskTest, NestedException) {
    try {
        run_task(nested_throw());
        FAIL() << "Expected logic_error";
    } catch (const std::logic_error& e) {
        EXPECT_STREQ(e.what(), "nested");
    }
}

} // namespace
