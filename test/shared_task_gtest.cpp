/// GoogleTest unit tests for coronet::shared_task<T>

#include <coronet/shared_task.hpp>
#include <coronet/io_context.hpp>

#include <gtest/gtest.h>
#include <string>
#include <stdexcept>

using namespace coronet;

namespace {

// Helper: run a coroutine inside an io_context
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
TEST(SharedTaskTest, DefaultConstruction) {
    shared_task<int> t;
    EXPECT_FALSE(t.get_handle());
}

// ============================================================
// Test: basic coroutine execution and value retrieval
// ============================================================
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
