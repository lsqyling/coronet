/// coronet when_any test: 首个完成者胜出，返回 (index, variant)
/// First completion wins, returns (index, variant).
#include <coronet/all.hpp>
#include <cassert>
#include <variant>
#include <cstdio>
using namespace coronet;

task<int> f0() {
    printf("f0 start.\n"); fflush(stdout);
    co_await async::timeout(std::chrono::seconds{1});
    printf("f0 done.\n"); fflush(stdout);
    co_return 1;
}

shared_task<std::string> f1() {
    printf("f1 start.\n"); fflush(stdout);
    printf("f1 done.\n"); fflush(stdout);
    co_return "f1 Great!";
}

task<void> f2() {
    printf("f2 start.\n"); fflush(stdout);
    co_await async::timeout(std::chrono::seconds{2});
    printf("f2 done.\n"); fflush(stdout);
    co_return;
}

task<> run_and_stop(io_context& ctx) {
    auto [idx, var] = co_await any(f0(), f1(), f2());
    // f1 立即完成，应为胜者 / f1 completes immediately, should win
    assert(idx == 1);
    printf("get the result of f%zu: ", idx);
    std::visit(
        overload{
            [](std::monostate) { printf("(void)\n"); },
            [](int x) { printf("%d : int\n", x); },
            [](const std::string &s) { printf("%s : string\n", s.c_str()); },
        },
        var
    );
    printf("when_any test PASSED\n");
    ctx.can_stop();
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    io_context ctx;
    ctx.co_spawn(run_and_stop(ctx));
    ctx.start();
    ctx.join();
    return 0;
}
