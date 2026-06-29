/// coronet when_all test: 等待所有任务完成，通过结构化绑定获取结果
/// Wait for all tasks, get results via structured binding.
#include <coronet/all.hpp>
#include <cassert>
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
    auto [r0, r1] = co_await all(f0(), f1(), f2());
    // 验证结果 / Verify results
    assert(r0 == 1);
    assert(r1 == "f1 Great!");
    printf("get the result of f0: %d\n", r0);
    printf("get the result of f1: %s\n", r1.c_str());
    printf("when_all test PASSED\n");
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
