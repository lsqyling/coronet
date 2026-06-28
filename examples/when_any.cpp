/// coronet when_any demo: first completion wins, returns (index, variant)
#include <coronet/all.hpp>
#include <iostream>
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

task<> run() {
    auto [idx, var] = co_await any(f0(), f1(), f2());
    std::cout << "get the result of f" << idx << ": ";
    std::visit(
        overload{
            [](std::monostate) { std::cout << "(void)\n"; },
            [](int x) { std::cout << x << " : int\n"; },
            [](const std::string &s) { std::cout << s << " : string\n"; },
        },
        var
    );
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    io_context ctx;
    ctx.co_spawn(run());
    ctx.start();
    ctx.join();
    return 0;
}
