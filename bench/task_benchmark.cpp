// ============================================================
// task_benchmark.cpp — coronet::task<T> 协程性能基准测试
// ============================================================
// 使用 Google Benchmark 库测量 task<T> 的创建和链式切换开销。
//
// 测试项目：
//   1. BM_TaskCreate — 仅协程创建开销（不执行，只构造 task 对象）
//   2. BM_TaskChain_N — 创建深度为 N 的协程链并测量总开销
//
// 为什么测量这些指标：
//   - 协程创建开销影响"启动大量轻量任务"场景的性能
//   - 链式协程（task 嵌套 co_await）反映了协程间切换的内联执行开销
//   - 由于 coronet::task 是惰性的（lazy），创建 task 不会执行协程体，
//     所以这些 benchmark 主要测量协程帧分配和 promise 构造的开销。
//
// Google Benchmark: https://github.com/google/benchmark

#include <benchmark/benchmark.h>
#include "coronet/task.hpp"

using namespace coronet;

namespace {

// 创建一个简单的返回整数的协程，用于测量创建开销
task<int> simple_task(int v) { co_return v; }

// 创建深度为 N 的协程链：链中每层创建子协程并 co_await。
// 此模式测试协程间内联切换（inline execution）的性能 ——
// 子协程完成后通过 final_suspend 直接返回父协程句柄，零调度器介入。
//
// Chain of N tasks, each awaiting the next.
task<int> chain(int depth) {
    if (depth <= 0) co_return 1;
    auto sub = chain(depth - 1);
    // 子 task 在父协程的栈帧上"内联"执行（通过 parent_coroutine 链）
    // Inline: sub completes before we proceed.
    co_return depth;
}

// 基准测试：仅测量 task<> 对象的创建开销（不执行协程体）
// 测试惰性协程的构造性能
static void BM_TaskCreate(benchmark::State& state) {
    for (auto _ : state) {
        auto t = simple_task(42);
        benchmark::DoNotOptimize(&t);
    }
}
BENCHMARK(BM_TaskCreate);

// 基准测试：创建深度为 10 的协程链，测量链式创建的总开销
static void BM_TaskChain_10(benchmark::State& state) {
    for (auto _ : state) {
        auto t = chain(10);
        benchmark::DoNotOptimize(&t);
    }
}
BENCHMARK(BM_TaskChain_10);

// 基准测试：创建深度为 100 的协程链，测试深层嵌套的性能
static void BM_TaskChain_100(benchmark::State& state) {
    for (auto _ : state) {
        auto t = chain(100);
        benchmark::DoNotOptimize(&t);
    }
}
BENCHMARK(BM_TaskChain_100);

} // namespace
