/// coronet when_all demo: wait for all tasks, get results via structured binding
#include <coronet/all.hpp>
#include <iostream>
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

task<> run() {
    auto [r0, r1] = co_await all(f0(), f1(), f2());
    std::cout << "get the result of f0: " << r0 << "\n";
    std::cout << "get the result of f1: " << r1 << "\n";
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    io_context ctx;
    ctx.co_spawn(run());
    ctx.start();
    ctx.join();
    return 0;
}
