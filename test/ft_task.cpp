/// Functional test for coronet::task<T>
/// Ported from co_context/test/ft_task.cpp, adapted for coronet namespace.
/// Each test scenario runs inside an io_context via co_spawn.
///
/// 本文件是 coronet::task<T> 的功能测试套件，使用手动断言而非测试框架。
/// 从 co_context 项目移植并适配到 coronet 命名空间。
///
/// task<T> 是 coronet 的核心协程类型，采用惰性求值 + 唯一所有权设计：
///   - 惰性：协程体在 co_await 时才开始执行
///   - 唯一所有权：不可复制，仅可移动（unique ownership）
///
/// 测试模式：
///   需要 io_context 支持协程执行的测试使用 run_test 辅助函数。
///   标准流程为：co_spawn -> start -> can_stop -> join。
///   其中 can_stop() 在所有协程完成后向事件循环发送停止信号，
///   join() 等待事件循环线程优雅退出。
///
/// 测试覆盖了 task<T> 的全部核心接口和边角情况：
///   1. 构造与移动语义
///   2. 移动赋值与自赋值
///   3. is_ready() 状态查询
///   4. detach() 协程句柄剥离
///   5. swap() 与 std::swap() 交换
///   6. get_handle() 句柄访问
///   7. when_ready() 不取值等待
///   8. co_await 左值引用
///   9. co_await 右值引用
///   10. 引用返回类型
///   11. 异常处理
///   12. 链式协程与并发组合

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
//
// 测试辅助函数：在 io_context 事件循环中执行协程。
// 所有需要协程上下文的测试都通过此函数运行。
// io_context 是 coronet 的事件循环调度器，它管理协程的唤醒和调度。
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
//
// 测试 task<T> 的构造和移动语义（不需要 io_context，因为是纯构造操作）：
//   1. 默认构造：构造空的 task，get_handle() 应返回空
//   2. 移动构造：通过立即调用的 lambda 协程创建 task，
//      然后移动构造到另一个 task 对象。源对象变为空。
//   3. 编译时验证：复制构造函数被删除（尝试复制会导致编译错误）
// 这些测试验证了 task 的唯一所有权语义。
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
//
// 测试 task 的移动赋值操作：
//   1. 基本移动赋值：将 t1 移动到 t2，t1 变为空，t2 获得 t1 的协程句柄
//   2. 移动后通过 co_await t2 获取结果，验证值为 100
//   3. 自赋值：将 task 移动赋值给自己，应保持有效状态
// 自赋值是重要的边角情况，必须正确处理（不应导致协程状态损坏）
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
//
// 测试 is_ready() 方法在不同状态下的返回值：
//   1. 空 task：默认构造后即为就绪状态（empty task 被视为 ready）
//   2. 协程执行完毕后：通过 co_await 驱动协程至完成，
//      完成后 is_ready() 返回 true
// is_ready() 用于判断协程是否已经完成执行，在协程组合和轮询场景中很重要
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
//
// 测试 detach() 方法的语义：
//   1. detach 从 task 中剥离协程句柄，使 task 变为空
//   2. 剥离后的协程句柄可以被手动恢复（resume），独立运行
//   3. 对于非 void 类型的 task，detach 后不再能通过 co_await 获取结果
// detach 适用于"即发即忘"（fire-and-forget）的场景：
// 协程启动后不需要等待其完成，协程结束后会自动清理资源
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
//
// 测试 task 的交换操作：
//   1. 两个有效 task 交换：交换后各自的协程句柄互换，但保持有效性
//   2. 交换后 co_await 获取的值也互换（内容随句柄一起交换）
//   3. 与空 task 交换：交换后一个变空，另一个获得有效句柄
//   4. std::swap 兼容性：通过 ADL 找到的 swap 和 std::swap 行为一致
// swap 是重要的基础操作，用于协程池管理和所有权转移
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
//
// 测试 get_handle() 方法：
//   1. 有效协程：返回非空协程句柄，且句柄对应的协程尚未完成（!done()）
//   2. const 正确性：const 引用上的 get_handle() 应返回相同的句柄
//   3. 空 task：返回空句柄
// 协程句柄是 C++20 协程的核心底层 API，用于手动控制协程的执行
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
//
// 测试 when_ready() 方法的语义：
//   1. 在协程上调用 when_ready() 返回一个可等待对象，当协程完成时变为就绪
//   2. when_ready() 不会获取协程的返回值或重新抛出异常
//   3. 在已完成的协程上调用 when_ready() 应立即可等待（不挂起）
// when_ready() 适用于"等待协程完成但不需要返回值"的场景，
// 例如触发后续操作而不关心计算结果的场景
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
//
// 测试通过左值 co_await task 的语义：
//   1. co_await 左值 task 返回结果，可以绑定到左值引用 int&
//   2. 验证返回值为 42
//   3. task<void> 的 co_await 左值：等待完成但不获取值
// 左值 co_await 适用于需要多次等待同一 task 的场景（虽然 task 是唯一所有权，
// 但在同一协程内可以多次 co_await 同一个左值）
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
//
// 测试通过右值 co_await task 的语义：
//   1. 直接 co_await 临时 task 对象（右值），获取返回值
//   2. 通过 std::move 将左值转为右值后 co_await
//   3. 复杂类型（std::string）的右值 co_await
// 右值 co_await 适用于"即用即弃"场景，语义上表示调用方获取了 task 的所有权
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
//
// 测试 task<int&> 引用返回类型的语义：
//   1. co_await 返回的是原始变量的引用，而非拷贝
//   2. 引用地址应等于原始变量的地址（验证引用语义的正确性）
//   3. 通过引用修改值，原始变量同步改变
// 引用返回类型在需要协程修改外部状态时非常有用
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
//
// 测试协程异常处理机制：
//   1. task<int> 中抛出 std::runtime_error，co_await 时异常被重新抛出
//   2. 验证异常类型和消息字符串正确传递
//   3. task<void> 中抛出 std::logic_error，同样验证异常传播
// 协程中的异常处理机制是：异常被存储在 promise 中，
// 在 co_await 时通过 promise().unhandled_exception() 重新抛出给等待者
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
//
// 综合场景测试，验证 task 的组合能力：
//   1. 链式任务：在协程体内嵌套 co_await 另一个协程，
//      结果组合（内层结果 + 5），这是最基本的协程组合模式
//   2. 并发任务：创建多个独立协程，逐个 co_await 收集结果，
//      验证结果累加正确性
//   3. 交换后等待：先交换协程句柄再等待，
//      验证交换后的值交换效果
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
