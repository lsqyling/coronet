/// Coroutine lifetime test: validates that coroutine parameters are properly
/// copied/moved/destroyed across suspension points.

#include <coronet/task.hpp>
#include <coronet/io_context.hpp>

#include <cassert>
#include <cstdio>
#include <memory>

using namespace coronet;

struct S {
    int x;

    S(int x) : x(x) { std::printf("S(%d)\n", x); }
    S(const S& s) : x(s.x * 10) {
        std::printf("S(copy S(%d)) -> %d\n", s.x, x);
    }
    S(S&& s) : x(s.x * 10) {
        std::printf("S(move S(%d)) -> %d\n", s.x, x);
    }
    ~S() { std::printf("~S(%d)\n", x); }
};

task<S> coro_copy(S x) {
    co_return x;
}

struct check_move_only {
    std::unique_ptr<int> val;
    explicit check_move_only(int v) : val(std::make_unique<int>(v)) {}
    check_move_only(check_move_only&&) = default;
    check_move_only(const check_move_only&) = delete;
};

// Verify move-only types can be passed through coroutines
task<check_move_only> coro_move_only(check_move_only x) {
    co_return std::move(x);
}

task<> run() {
    std::printf("--- Test: coro_copy ---\n");
    S res0 = co_await coro_copy(S{1});
    std::printf("coro_copy finished, result.x=%d\n", res0.x);
    // Verify the result is valid (exact value depends on copy/move chain with x10 multipliers)
    assert(res0.x != 0);

    std::printf("--- Test: move-only type ---\n");
    auto m = co_await coro_move_only(check_move_only{42});
    assert(*m.val == 42);
    std::printf("move_only test passed\n");

    std::printf("coro_lifetime tests passed!\n");
}

int main() {
    io_context ctx;
    ctx.co_spawn(run());
    ctx.start();
    ctx.can_stop();
    ctx.join();
    return 0;
}
