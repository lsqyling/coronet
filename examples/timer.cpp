/// coronet timer demo: periodic tasks with timeout
#include <coronet/io_context.hpp>
#include <coronet/async_io.hpp>

using namespace coronet;

task<> cycle(int sec, const char *message) {
    while (true) {
        co_await async::timeout(std::chrono::seconds{sec});
        printf("%s\n", message); fflush(stdout);
    }
}

task<> cycle_rel(int sec, const char *message) {
    while (true) {
        co_await async::timeout(std::chrono::seconds{sec});
        printf("%s\n", message); fflush(stdout);
    }
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    io_context ctx;
    ctx.co_spawn(cycle(1, "1 sec"));
    ctx.co_spawn(cycle_rel(1, "1 sec [rel]"));
    ctx.co_spawn(cycle(3, "\t3 sec"));
    ctx.start();
    ctx.join();
    return 0;
}
