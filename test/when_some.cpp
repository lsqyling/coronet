/// coronet when_some test: 等待 N 个任务完成
/// Wait for N tasks to complete.
#include <coronet/all.hpp>
#include <cassert>
#include <variant>
#include <cstdio>
using namespace coronet;

task<int> f0() {
    printf("f0 start.\n"); fflush(stdout);
    co_await async::timeout(std::chrono::seconds{1});
    printf("f0 done.\n"); fflush(stdout);
    co_return 0;
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
    auto results = co_await some(2, f0(), f1(), f2());
    // 应有 2 个结果（f1 立即完成 + f0 1s 后完成）/ Should have 2 results
    assert(results.size() == 2);
    for (const auto &[idx, var] : results) {
        printf("get the result of f%zu: ", idx);
        std::visit(
            overload{
                [](std::monostate) { printf("(void)\n"); },
                [](int x) { printf("%d : int\n", x); },
                [](const std::string &s) { printf("%s : string\n", s.c_str()); },
            },
            var
        );
    }
    printf("when_some test PASSED\n");
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
