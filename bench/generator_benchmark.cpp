// ============================================================
// generator_benchmark.cpp — coronet::generator<T> 协程性能基准测试
// ============================================================
// 使用 Google Benchmark 库测量 generator<T> 的 co_yield 切换开销。
//
// 测试项目：
//   1. BM_GeneratorYield — 在不同大小的生成器上迭代，测量 yield 切换开销
//
// 为什么测量这个指标：
//   - generator 是拉取式（pull-based）协程，每次 co_yield 都涉及
//     从消费者到生成器再到消费者的上下文切换
//   - 测量不同规模（100/1000/10000 次 yield）的总耗时，
//     可以计算出单次 yield 的平均开销
//   - 这对理解生成器在数据流管线中的性能影响很重要
//
// Google Benchmark: https://github.com/google/benchmark

#include <benchmark/benchmark.h>
#include "coronet/generator.hpp"

using namespace coronet;

namespace {

// 创建一个生成 0 到 n-1 的整数生成器
generator<int> range(int n) {
    for (int i = 0; i < n; ++i) {
        co_yield i;
    }
}

// 基准测试：在不同规模的生成器上迭代求和，测量每次 yield 的平均开销
// Args: 100（100 次 yield）、1000（1000 次）、10000（10000 次）
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
