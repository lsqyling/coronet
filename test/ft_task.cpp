/// Functional test for coronet::task<T>
/// Ported from co_context/test/ft_task.cpp, adapted for coronet namespace.
/// Each test scenario runs inside an io_context via co_spawn.

#include <coronet/task.hpp>
#include <coronet/io_context.hpp>

#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

using namespace coronet;

#define TEST_PASS() std::printf("  PASS\n")

// ============================================================
// Helper: run a test coroutine inside an io_context
// ============================================================
template<typename F>
static void run_test(F func) {
    io_context ctx;
    ctx.co_spawn(func());
    ctx.start();
    ctx.can_stop();
    ctx.join();
}

// ============================================================
// 1. Construction and move tests (no io_context needed)
// ============================================================
static void test_construction() {
    std::printf("=== Test: Construction ===\n");

    // Default construction
    task<int> t1;
    assert(!t1.get_handle());
    TEST_PASS();

    // Move construction
    task<int> t2 = []() -> task<int> {
        co_return 42;
    }();
    task<int> t3(std::move(t2));
    assert(t3.get_handle());
    assert(!t2.get_handle());
    TEST_PASS();

    // Copy is deleted (compile-time check only)
    std::printf("  Construction tests passed!\n\n");
}

// ============================================================
// 2. Move assignment
// ============================================================
task<> coro_move_assignment() {
    std::printf("=== Test: Move Assignment ===\n");

    task<int> t1 = []() -> task<int> { co_return 100; }();
    task<int> t2;
    t2 = std::move(t1);
    assert(t2.get_handle());
    assert(!t1.get_handle());
    int result = co_await t2;
    assert(result == 100);
    TEST_PASS();

    // Self-assignment
    task<int> t3 = []() -> task<int> { co_return 200; }();
    t3 = std::move(t3);
    assert(t3.get_handle());
    TEST_PASS();

    std::printf("  Move assignment tests passed!\n\n");
}

// ============================================================
// 3. is_ready()
// ============================================================
task<> coro_is_ready() {
    std::printf("=== Test: is_ready() ===\n");

    // Empty task is ready
    task<int> t1;
    assert(t1.is_ready());
    TEST_PASS();

    // After co_await, task is ready
    task<int> t2 = []() -> task<int> { co_return 42; }();
    assert(t2.get_handle());  // handle exists before co_await
    co_await t2;
    assert(t2.is_ready());
    TEST_PASS();

    std::printf("  is_ready() tests passed!\n\n");
}

// ============================================================
// 4. detach()
// ============================================================
task<> coro_detach() {
    std::printf("=== Test: detach() ===\n");

    bool executed = false;

    // task<void> detach
    task<> t1 = [&executed]() -> task<> {
        executed = true;
        co_return;
    }();
    auto h1 = t1.get_handle();
    t1.detach();
    assert(!t1.get_handle());  // handle cleared after detach
    // Resume the detached handle so it runs to completion
    h1.resume();
    assert(executed);
    TEST_PASS();

    // Non-void task detach
    task<int> t2 = []() -> task<int> { co_return 999; }();
    t2.detach();
    assert(!t2.get_handle());
    TEST_PASS();

    std::printf("  detach() tests passed!\n\n");
    co_return;
}

// ============================================================
// 5. swap()
// ============================================================
task<> coro_swap() {
    std::printf("=== Test: swap() ===\n");

    task<int> t1 = []() -> task<int> { co_return 10; }();
    task<int> t2 = []() -> task<int> { co_return 20; }();
    swap(t1, t2);
    assert(t1.get_handle() && t2.get_handle());
    int r1 = co_await t1;
    int r2 = co_await t2;
    assert(r1 == 20 && r2 == 10);
    TEST_PASS();

    // Swap with empty
    task<int> t3 = []() -> task<int> { co_return 30; }();
    task<int> t4;
    swap(t3, t4);
    assert(!t3.get_handle() && t4.get_handle());
    TEST_PASS();

    // std::swap
    task<int> t5 = []() -> task<int> { co_return 50; }();
    task<int> t6 = []() -> task<int> { co_return 60; }();
    std::swap(t5, t6);
    int r5 = co_await t5;
    int r6 = co_await t6;
    assert(r5 == 60 && r6 == 50);
    TEST_PASS();

    std::printf("  swap() tests passed!\n\n");
}

// ============================================================
// 6. get_handle()
// ============================================================
task<> coro_get_handle() {
    std::printf("=== Test: get_handle() ===\n");

    task<int> t1 = []() -> task<int> { co_return 42; }();
    auto handle = t1.get_handle();
    assert(handle);
    assert(!handle.done());
    TEST_PASS();

    // const correctness
    const task<int>& t1_ref = t1;
    auto const_handle = t1_ref.get_handle();
    assert(const_handle == handle);
    TEST_PASS();

    // Empty task
    task<int> t2;
    assert(!t2.get_handle());
    TEST_PASS();

    std::printf("  get_handle() tests passed!\n\n");
    co_return;
}

// ============================================================
// 7. when_ready()
// ============================================================
task<> coro_when_ready() {
    std::printf("=== Test: when_ready() ===\n");

    task<int> t = []() -> task<int> { co_return 100; }();
    co_await t.when_ready();
    assert(t.is_ready());
    TEST_PASS();

    // On already-completed task
    co_await t.when_ready();
    TEST_PASS();

    std::printf("  when_ready() tests passed!\n\n");
}

// ============================================================
// 8. co_await lvalue
// ============================================================
task<> coro_co_await_lvalue() {
    std::printf("=== Test: co_await (lvalue) ===\n");

    task<int> t = []() -> task<int> { co_return 42; }();
    int& result = co_await t;
    assert(result == 42);
    TEST_PASS();

    // task<void>
    task<> tv = []() -> task<> { co_return; }();
    co_await tv;
    TEST_PASS();

    std::printf("  co_await (lvalue) tests passed!\n\n");
}

// ============================================================
// 9. co_await rvalue
// ============================================================
task<> coro_co_await_rvalue() {
    std::printf("=== Test: co_await (rvalue) ===\n");

    int result = co_await []() -> task<int> {
        co_return 99;
    }();
    assert(result == 99);
    TEST_PASS();

    // Complex type
    task<std::string> t = []() -> task<std::string> {
        co_return std::string("Hello, Task!");
    }();
    std::string str = co_await std::move(t);
    assert(str == "Hello, Task!");
    TEST_PASS();

    std::printf("  co_await (rvalue) tests passed!\n\n");
}

// ============================================================
// 10. Reference return type
// ============================================================
task<> coro_reference_return() {
    std::printf("=== Test: Reference Return Type ===\n");

    int value = 42;
    task<int&> t = [&value]() -> task<int&> { co_return value; }();
    int& ref = co_await t;
    assert(ref == 42);
    assert(&ref == &value);
    TEST_PASS();

    // Modify through reference
    ref = 100;
    assert(value == 100);
    TEST_PASS();

    std::printf("  Reference return tests passed!\n\n");
}

// ============================================================
// 11. Exception handling
// ============================================================
task<> coro_exception_handling() {
    std::printf("=== Test: Exception Handling ===\n");

    // task<int> exception
    bool caught = false;
    task<int> t1 = []() -> task<int> {
        throw std::runtime_error("Test exception");
        co_return 0;
    }();
    try {
        co_await t1;
    } catch (const std::runtime_error& e) {
        assert(std::string(e.what()) == "Test exception");
        caught = true;
    }
    assert(caught);
    TEST_PASS();

    // task<void> exception
    caught = false;
    task<> t2 = []() -> task<> {
        throw std::logic_error("task<void> exception");
    }();
    try {
        co_await t2;
    } catch (const std::logic_error& e) {
        assert(std::string(e.what()) == "task<void> exception");
        caught = true;
    }
    assert(caught);
    TEST_PASS();

    std::printf("  Exception handling tests passed!\n\n");
}

// ============================================================
// 12. Chained & concurrent tasks
// ============================================================
task<> coro_comprehensive() {
    std::printf("=== Test: Comprehensive Scenarios ===\n");

    // Chained tasks
    auto result = co_await []() -> task<int> {
        auto inner = []() -> task<int> {
            co_return 10;
        }();
        co_return co_await inner + 5;
    }();
    assert(result == 15);
    TEST_PASS();

    // Multiple concurrent tasks
    task<int> t1 = []() -> task<int> { co_return 1; }();
    task<int> t2 = []() -> task<int> { co_return 2; }();
    task<int> t3 = []() -> task<int> { co_return 3; }();
    int r1 = co_await t1;
    int r2 = co_await t2;
    int r3 = co_await t3;
    assert(r1 + r2 + r3 == 6);
    TEST_PASS();

    // Swap then await
    task<int> a = []() -> task<int> { co_return 100; }();
    task<int> b = []() -> task<int> { co_return 200; }();
    swap(a, b);
    int ra = co_await a;
    int rb = co_await b;
    assert(ra == 200 && rb == 100);
    TEST_PASS();

    std::printf("  Comprehensive tests passed!\n\n");
}

// ============================================================
// Main
// ============================================================
int main() {
    std::printf("========================================\n");
    std::printf("   coronet::task<> Functional Test Suite\n");
    std::printf("========================================\n\n");

    // Test 1: Construction (no io_context needed)
    test_construction();

    // Tests 2-12: Run inside io_context
    try {
        run_test(coro_move_assignment);
        run_test(coro_is_ready);
        run_test(coro_detach);
        run_test(coro_swap);
        run_test(coro_get_handle);
        run_test(coro_when_ready);
        run_test(coro_co_await_lvalue);
        run_test(coro_co_await_rvalue);
        run_test(coro_reference_return);
        run_test(coro_exception_handling);
        run_test(coro_comprehensive);

        std::printf("========================================\n");
        std::printf("   ALL TESTS PASSED!\n");
        std::printf("========================================\n");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "TEST FAILED: %s\n", e.what());
        return 1;
    }
}
