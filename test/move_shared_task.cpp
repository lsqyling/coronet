/// Test coronet::shared_task multi-await semantics.
/// Ported from co_context/test/move_shared_task.cpp

#include <coronet/shared_task.hpp>
#include <coronet/io_context.hpp>

#include <cassert>
#include <cstdio>
#include <string>
#include <utility>

using namespace coronet;

shared_task<std::string> make_shared_str() {
    co_return "shared_task_value";
}

shared_task<int> make_shared_int() {
    co_return 42;
}

task<> run() {
    std::printf("=== Test: shared_task ===\n");

    // Test 1: multiple co_await on same shared_task returns same value
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
    std::string s3 = co_await std::move(t_str);
    std::printf("  moved: '%s'\n", s3.c_str());
    assert(s3 == "shared_task_value");
    std::printf("  PASS\n");

    // Test 3: await again after move (still valid, returns stored value)
    std::string s4 = co_await t_str;
    std::printf("  await after move: '%s'\n", s4.c_str());
    assert(s4 == "shared_task_value");
    std::printf("  PASS\n");

    // Test 4: move result into variable
    std::string s5 = std::move(co_await t_str);
    std::printf("  std::moved result: '%s'\n", s5.c_str());
    assert(s5 == "shared_task_value");
    std::printf("  PASS\n");

    // Test 5: copy semantics (note: value was moved in test 4 step 5,
    // so the promise's value is now empty)
    auto t2 = t_str;  // shared_task is copyable
    std::string s6 = co_await t2;
    // After std::move(co_await t) above, the internal value is moved-from
    assert(s6.empty());
    std::printf("  PASS\n");

    // Test 6: int value type
    auto t_int = make_shared_int();
    int v1 = co_await t_int;
    assert(v1 == 42);
    int v2 = co_await t_int;
    assert(v2 == 42);
    std::printf("  PASS\n");

    std::printf("  All shared_task tests passed!\n");
}

int main() {
    io_context ctx;
    ctx.co_spawn(run());
    ctx.start();
    ctx.can_stop();
    ctx.join();
    return 0;
}
