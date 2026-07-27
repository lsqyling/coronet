/**
 * debug_void_exception.cpp — 最小化复现 task<void> 异常崩溃
 *
 * 测试矩阵：
 *   A. 仅 task<void> 异常（无前序 task<int> 测试）
 *   B. task<int> 异常后接 task<void> 异常
 *   C. task<void> 异常后接 task<void> 异常（连续两次 void）
 */
#include <coronet/all.hpp>
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace coronet;

task<int> throw_int(const char* msg) {
    throw std::runtime_error(msg);
    co_return 0;
}

task<> throw_void(const char* msg) {
    throw std::logic_error(msg);
}

task<> stopper(io_context& ctx, int sec) {
    co_await async::timeout(std::chrono::seconds(sec));
    ctx.can_stop();
}

// Test A: 仅 task<void> 异常 — 带详细日志
task<> noop_void() {
    co_return;
}

// 带参数但不抛异常
task<> void_with_param(const char* msg) {
    (void)msg;
    co_return;
}

// 无参数但抛异常 — 带 co_return
task<> throw_void_noparam() {
    std::fprintf(stderr, "  [DBG!!] throw_void_noparam BODY IS RUNNING\n"); std::fflush(stderr);
    throw std::logic_error("no param");
    co_return;  // unreachable but helps MSVC
}

// 有局部变量但不抛
task<> void_with_local() {
    volatile int x = 42;
    (void)x;
    co_return;
}

task<> test_a_only_void() {
    // 1. 无参数无异常
    std::fprintf(stderr, "  [1] noop_void...\n"); std::fflush(stderr);
    { task<> t0 = noop_void(); co_await t0; std::fprintf(stderr, "  [1] ok\n"); std::fflush(stderr); }

    // 2. 有参数无异常
    std::fprintf(stderr, "  [2] void_with_param...\n"); std::fflush(stderr);
    { task<> t1 = void_with_param("hello"); co_await t1; std::fprintf(stderr, "  [2] ok\n"); std::fflush(stderr); }

    // 3. 有局部变量无异常
    std::fprintf(stderr, "  [3] void_with_local...\n"); std::fflush(stderr);
    { task<> t2 = void_with_local(); co_await t2; std::fprintf(stderr, "  [3] ok\n"); std::fflush(stderr); }

    // 3b. task<int> that throws (no param)
    std::fprintf(stderr, "  [3b] throw_int_noparam...\n"); std::fflush(stderr);
    {
        task<int> ti = throw_int("int throw");
        std::fprintf(stderr, "  [3b] created ok\n"); std::fflush(stderr);
        try { co_await ti; } catch (const std::runtime_error& e) {
            std::fprintf(stderr, "  [3b] caught: %s\n", e.what()); std::fflush(stderr);
        }
        std::fprintf(stderr, "  [3b] ok\n"); std::fflush(stderr);
    }

    // 4. 无参数但抛异常 (task<void>)
    std::fprintf(stderr, "  [4] throw_void_noparam...\n"); std::fflush(stderr);
    { task<> t3 = throw_void_noparam();
      std::fprintf(stderr, "  [4] created, co_await...\n"); std::fflush(stderr);
      try { co_await t3; } catch (const std::logic_error& e) {
        std::fprintf(stderr, "  [4] caught: %s\n", e.what()); std::fflush(stderr);
      }
      std::fprintf(stderr, "  [4] ok\n"); std::fflush(stderr);
    }

    // 5. 有参数且抛异常（原始崩溃点）
    std::fprintf(stderr, "  [5] throw_void(\"test\")...\n"); std::fflush(stderr);
    { task<> t4 = throw_void("test");
      std::fprintf(stderr, "  [5] created, co_await...\n"); std::fflush(stderr);
      try { co_await t4; } catch (const std::logic_error& e) {
        std::fprintf(stderr, "  [5] caught: %s\n", e.what()); std::fflush(stderr);
      }
      std::fprintf(stderr, "  [5] ok\n"); std::fflush(stderr);
    }
}

// Test B: task<int> 异常后接 task<void> 异常
task<> test_b_int_then_void() {
    // task<int> 异常
    std::fprintf(stderr, "  [B] creating throw_int...\n"); std::fflush(stderr);
    {
        task<int> t1 = throw_int("int first");
        try {
            co_await t1;
        } catch (const std::runtime_error& e) {
            std::fprintf(stderr, "  [B] int caught: %s\n", e.what()); std::fflush(stderr);
        }
    }
    std::fprintf(stderr, "  [B] t1 destroyed\n"); std::fflush(stderr);

    // task<void> 异常
    std::fprintf(stderr, "  [B] creating throw_void...\n"); std::fflush(stderr);
    {
        task<> t2 = throw_void("void second");
        std::fprintf(stderr, "  [B] void created, co_await...\n"); std::fflush(stderr);
        try {
            co_await t2;
        } catch (const std::logic_error& e) {
            std::fprintf(stderr, "  [B] void caught: %s\n", e.what()); std::fflush(stderr);
        }
    }
    std::fprintf(stderr, "  [B] t2 destroyed\n"); std::fflush(stderr);
}

// Test C: 连续两次 task<void> 异常
task<> test_c_two_void() {
    std::fprintf(stderr, "  [C] first throw_void...\n"); std::fflush(stderr);
    {
        task<> t1 = throw_void("void first");
        try { co_await t1; } catch (const std::logic_error& e) {
            std::fprintf(stderr, "  [C] first caught: %s\n", e.what()); std::fflush(stderr);
        }
    }
    std::fprintf(stderr, "  [C] second throw_void...\n"); std::fflush(stderr);
    {
        task<> t2 = throw_void("void second");
        try { co_await t2; } catch (const std::logic_error& e) {
            std::fprintf(stderr, "  [C] second caught: %s\n", e.what()); std::fflush(stderr);
        }
    }
    std::fprintf(stderr, "  [C] done\n"); std::fflush(stderr);
}

void run(task<> (*fn)()) {
    io_context ctx;
    ctx.co_spawn(fn());
    ctx.co_spawn(stopper(ctx, 5));
    ctx.start();
    ctx.join();
}

int main() {
    // Redirect stderr to a file for reliable output
    FILE* logf = std::freopen("debug_void_exception.log", "w", stderr);
    (void)logf;

    // Test 0: 在主线程直接创建 task<void> that throws（不经过 io_context）
    std::printf("=== Test 0: direct creation (no io_context) ===\n");
    std::fprintf(stderr, "  [0] creating throw_void_noparam on main thread...\n"); std::fflush(stderr);
    {
        task<> t = throw_void_noparam();
        std::fprintf(stderr, "  [0] created, destroying...\n"); std::fflush(stderr);
        // 不 co_await，直接析构 — 测试协程帧创建+析构
    }
    std::fprintf(stderr, "  [0] destroyed ok\n"); std::fflush(stderr);
    std::printf("=== Test 0 PASSED ===\n\n");

    std::printf("=== Test A: only task<void> exception ===\n");
    run(test_a_only_void);
    std::printf("=== Test A PASSED ===\n\n");

    std::printf("=== Test B: task<int> then task<void> ===\n");
    run(test_b_int_then_void);
    std::printf("=== Test B PASSED ===\n\n");

    std::printf("=== Test C: two task<void> exceptions ===\n");
    run(test_c_two_void);
    std::printf("=== Test C PASSED ===\n\n");

    std::printf("ALL TESTS PASSED\n");
    return 0;
}
