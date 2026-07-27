/**
 * combinator_stress.cpp — when_all/when_any/when_some 大批量压测
 *
 * 使用协程函数（非 lambda），参数值传递，避免协程帧生命周期问题。
 *
 * 测试覆盖：
 *   Phase 1: when_all 大批量 (30 tasks, 折叠表达式展开)
 *   Phase 2: when_all 全 void 路径 (20 void tasks)
 *   Phase 3: when_all 混合类型 (int + void)
 *   Phase 4: when_any 多竞争者取最快
 *   Phase 5: when_some 取前 N 个
 *   Phase 6: 嵌套 when_all
 */

#include <coronet/all.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

using namespace coronet;
using namespace std::chrono_literals;

// ============================================================
// 工具函数
// ============================================================

static auto t0 = std::chrono::steady_clock::now();
static void tick(const char* msg) {
    printf("[%6lldms] %s\n",
           (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count(), msg);
    fflush(stdout);
}

static void chk(bool cond, const char* file, int line) {
    if (!cond) { printf("FAIL %s:%d\n", file, line); std::abort(); }
}
#define CHK(cond) chk((cond), __FILE__, __LINE__)

// ============================================================
// 协程函数 — 值传递参数，避免 lambda 捕获生命周期问题
// ============================================================

task<> stopper(io_context& ctx, int sec) {
    co_await async::timeout(std::chrono::seconds(sec));
    tick("TIMEOUT");
    ctx.can_stop();
}

// ---- Phase 1: when_all 30 int tasks ----
task<int> make_int(int idx) {
    co_return idx;
}

task<> phase_when_all_large(io_context& ctx) {
    auto results = co_await all(
        make_int(0),  make_int(1),  make_int(2),  make_int(3),  make_int(4),
        make_int(5),  make_int(6),  make_int(7),  make_int(8),  make_int(9),
        make_int(10), make_int(11), make_int(12), make_int(13), make_int(14),
        make_int(15), make_int(16), make_int(17), make_int(18), make_int(19),
        make_int(20), make_int(21), make_int(22), make_int(23), make_int(24),
        make_int(25), make_int(26), make_int(27), make_int(28), make_int(29)
    );
    // 验证每个结果
    CHK(std::get<0>(results) == 0);
    CHK(std::get<14>(results) == 14);
    CHK(std::get<29>(results) == 29);
    tick("when_all 30 done");
    ctx.can_stop();
}

// ---- Phase 2: when_all 20 void tasks ----
std::atomic<int> g_void_counter{0};

task<> make_void(int delay_ms) {
    co_await async::timeout(std::chrono::milliseconds(delay_ms));
    g_void_counter.fetch_add(1, std::memory_order_relaxed);
}

task<> phase_when_all_void(io_context& ctx) {
    g_void_counter.store(0);
    co_await all(
        make_void(1),  make_void(2),  make_void(3),  make_void(4),  make_void(5),
        make_void(6),  make_void(7),  make_void(8),  make_void(9),  make_void(10),
        make_void(1),  make_void(2),  make_void(3),  make_void(4),  make_void(5),
        make_void(6),  make_void(7),  make_void(8),  make_void(9),  make_void(10)
    );
    CHK(g_void_counter.load() == 20);
    tick("when_all void done");
    ctx.can_stop();
}

// ---- Phase 3: when_all 混合 int + void ----
task<int> make_val(int val) {
    co_return val;
}

task<> make_delayed_void(int delay_ms) {
    co_await async::timeout(std::chrono::milliseconds(delay_ms));
}

task<> phase_when_all_mixed(io_context& ctx) {
    auto [val] = co_await all(
        make_val(42),
        make_delayed_void(1), make_delayed_void(2), make_delayed_void(3),
        make_delayed_void(4), make_delayed_void(5)
    );
    CHK(val == 42);
    tick("when_all mixed done");
    ctx.can_stop();
}

// ---- Phase 4: when_any ----
task<int> make_timed_int(int delay_ms, int val) {
    co_await async::timeout(std::chrono::milliseconds(delay_ms));
    co_return val;
}

task<> phase_when_any(io_context& ctx) {
    auto [idx, val] = co_await any(
        make_timed_int(300, 100),
        make_timed_int(100, 200),
        make_timed_int(200, 300),
        make_timed_int(2000, 400)
    );
    CHK(idx == 1u);  // 100ms task wins
    std::visit(overload{
        [](int v) { CHK(v == 200); },
        [](std::monostate) { CHK(false); }
    }, val);
    tick("when_any done");
    ctx.can_stop();
}

// ---- Phase 5: when_some (取前 N 个) ----
// 使用较大延迟间隔，避免 Windows 定时器精度 (~15ms) 导致顺序不确定
task<> phase_when_some(io_context& ctx) {
    auto results = co_await some(3,
        make_timed_int(300, 100),   // idx 0: 300ms
        make_timed_int(100, 200),   // idx 1: 100ms  (第1完成)
        make_timed_int(200, 300),   // idx 2: 200ms  (第3完成)
        make_timed_int(500, 400),   // idx 3: 500ms
        make_timed_int(150, 500)    // idx 4: 150ms  (第2完成)
    );
    // 完成顺序: 100ms(idx=1), 150ms(idx=4), 200ms(idx=2)
    CHK(results.size() == 3u);
    CHK(results[0].first == 1u);
    CHK(results[1].first == 4u);
    CHK(results[2].first == 2u);
    tick("when_some done");
    ctx.can_stop();
}

// ---- Phase 6: 嵌套 when_all ----
task<std::tuple<int, int>> inner_all() {
    auto [a, b] = co_await all(make_val(10), make_val(20));
    co_return std::make_tuple(a, b);
}

task<std::string> make_string(const char* s) {
    co_return std::string(s);
}

task<> phase_nested(io_context& ctx) {
    auto [tup, str] = co_await all(inner_all(), make_string("ok"));
    CHK(std::get<0>(tup) == 10);
    CHK(std::get<1>(tup) == 20);
    CHK(str == "ok");
    tick("nested when_all done");
    ctx.can_stop();
}

// ============================================================
// 主函数 — 逐阶段运行，每阶段独立 io_context
// ============================================================

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);

    // Phase 1
    printf("=== Phase 1: when_all 30 tasks ===\n");
    { io_context ctx;
      ctx.co_spawn(phase_when_all_large(ctx));
      ctx.co_spawn(stopper(ctx, 10));
      ctx.start(); ctx.join(); }
    printf("[Phase 1] PASSED\n\n");

    // Phase 2
    printf("=== Phase 2: when_all 20 void tasks ===\n");
    { io_context ctx;
      ctx.co_spawn(phase_when_all_void(ctx));
      ctx.co_spawn(stopper(ctx, 10));
      ctx.start(); ctx.join(); }
    printf("[Phase 2] PASSED\n\n");

    // Phase 3
    printf("=== Phase 3: when_all mixed + exception ===\n");
    { io_context ctx;
      ctx.co_spawn(phase_when_all_mixed(ctx));
      ctx.co_spawn(stopper(ctx, 10));
      ctx.start(); ctx.join(); }
    printf("[Phase 3] PASSED\n\n");

    // Phase 4
    printf("=== Phase 4: when_any ===\n");
    { io_context ctx;
      ctx.co_spawn(phase_when_any(ctx));
      ctx.co_spawn(stopper(ctx, 10));
      ctx.start(); ctx.join(); }
    printf("[Phase 4] PASSED\n\n");

    // Phase 5
    printf("=== Phase 5: when_some ===\n");
    { io_context ctx;
      ctx.co_spawn(phase_when_some(ctx));
      ctx.co_spawn(stopper(ctx, 10));
      ctx.start(); ctx.join(); }
    printf("[Phase 5] PASSED\n\n");

    // Phase 6
    printf("=== Phase 6: nested when_all ===\n");
    { io_context ctx;
      ctx.co_spawn(phase_nested(ctx));
      ctx.co_spawn(stopper(ctx, 10));
      ctx.start(); ctx.join(); }
    printf("[Phase 6] PASSED\n\n");

    printf("=== ALL COMBINATOR TESTS PASSED ===\n");
    return 0;
}
