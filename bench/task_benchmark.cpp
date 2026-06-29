// Benchmark for task<> coroutine creation and switching overhead.
#include <benchmark/benchmark.h>
#include "coronet/task.hpp"

using namespace coronet;

namespace {

task<int> simple_task(int v) { co_return v; }

// Chain of N tasks, each awaiting the next
task<int> chain(int depth) {
    if (depth <= 0) co_return 1;
    auto sub = chain(depth - 1);
    // Inline: sub completes before we proceed
    co_return depth;
}

static void BM_TaskCreate(benchmark::State& state) {
    for (auto _ : state) {
        auto t = simple_task(42);
        benchmark::DoNotOptimize(&t);
    }
}
BENCHMARK(BM_TaskCreate);

static void BM_TaskChain_10(benchmark::State& state) {
    for (auto _ : state) {
        auto t = chain(10);
        benchmark::DoNotOptimize(&t);
    }
}
BENCHMARK(BM_TaskChain_10);

static void BM_TaskChain_100(benchmark::State& state) {
    for (auto _ : state) {
        auto t = chain(100);
        benchmark::DoNotOptimize(&t);
    }
}
BENCHMARK(BM_TaskChain_100);

} // namespace
