// Benchmark for generator<> yield overhead.
#include <benchmark/benchmark.h>
#include "coronet/generator.hpp"

using namespace coronet;

namespace {

generator<int> range(int n) {
    for (int i = 0; i < n; ++i) {
        co_yield i;
    }
}

static void BM_GeneratorYield(benchmark::State& state) {
    int n = state.range(0);
    for (auto _ : state) {
        int sum = 0;
        for (int v : range(n)) {
            sum += v;
        }
        benchmark::DoNotOptimize(sum);
    }
}
BENCHMARK(BM_GeneratorYield)->Arg(100)->Arg(1000)->Arg(10000);

} // namespace
