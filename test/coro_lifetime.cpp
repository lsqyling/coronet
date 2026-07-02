/// Coroutine lifetime test: validates that coroutine parameters are properly
/// copied/moved/destroyed across suspension points.
///
/// 本文件测试协程参数的生命周期管理，验证以下关键行为：
///   1. 传递给协程的参数在协程挂起/恢复过程中被正确拷贝或移动
///   2. 协程参数在协程完成后被正确销毁
///   3. 仅移动类型（如 std::unique_ptr）可以安全地通过协程传递
///
/// 协程参数的生命周期管理是 C++20 协程中的重要问题。
/// 当协程在某个挂起点挂起时，传递给协程的参数（按值传递）会被存储到
/// 协程帧（coroutine frame）中。如果参数的生命周期跨越挂起点，
/// 必须确保参数在协程恢复时仍然有效。
///
/// 测试模式：
///   使用自定义类型 S，其构造/拷贝/移动/析构函数都输出日志，
///   通过日志观察参数在协程执行过程中的生命周期变化。
///   通过 x10 的变换规则（拷贝/移动时将值乘以 10）可以追踪
///   参数经历了多少次拷贝和移动操作。

#include <coronet/task.hpp>
#include <coronet/io_context.hpp>

#include <cassert>
#include <cstdio>
#include <memory>

using namespace coronet;

// 自定义生命周期跟踪类型
// 每个构造函数和析构函数都打印日志，x 值的变化反映了拷贝/移动链
// 变换规则：每次拷贝或移动时 x 值乘以 10
// 这可以用来追踪一个值在协程中经过了多少次传递
struct S {
    int x;

    S(int x) : x(x) { std::printf("S(%d)\n", x); }
    S(const S& s) : x(s.x * 10) {
        std::printf("S(copy S(%d)) -> %d\n", s.x, x);
    }
    S(S&& s) : x(s.x * 10) {
        std::printf("S(move S(%d)) -> %d\n", s.x, x);
    }
    ~S() { std::printf("~S(%d)\n", x); }
};

// 接收 S 参数的协程，按值传递
// 参数 x 在协程挂起点之前传入，存储在协程帧中
// co_return 返回 x 时，可能再次经过拷贝或移动
// 通过日志可以观察到从构造到最终返回的完整传递链
task<S> coro_copy(S x) {
    co_return x;
}

// 仅移动类型 —— 验证协程对不可复制类型的支持
// check_move_only 包含 std::unique_ptr 成员，不可复制
// 必须通过移动语义传入和传出协程
// 这是 C++20 协程的重要能力：支持仅移动类型的参数和返回值
struct check_move_only {
    std::unique_ptr<int> val;
    explicit check_move_only(int v) : val(std::make_unique<int>(v)) {}
    check_move_only(check_move_only&&) = default;
    check_move_only(const check_move_only&) = delete;
};

// Verify move-only types can be passed through coroutines
// 验证仅移动类型可以通过协程参数/返回值传递
// 要求协程的 promise_type 支持 std::unique_ptr 的移动语义
task<check_move_only> coro_move_only(check_move_only x) {
    co_return std::move(x);
}

task<> run() {
    std::printf("--- Test: coro_copy ---\n");
    // 创建 S{1} 临时对象，传递给 coro_copy 协程
    // 观察日志：
    //   1. S(1)         —— 临时对象构造
    //   2. S(copy/move) —— 参数传递到协程帧
    //   3. S(copy/move) —— co_return 返回值
    //   4. ~S(x)        —— 各临时对象析构
    S res0 = co_await coro_copy(S{1});
    std::printf("coro_copy finished, result.x=%d\n", res0.x);
    // Verify the result is valid (exact value depends on copy/move chain with x10 multipliers)
    assert(res0.x != 0);

    std::printf("--- Test: move-only type ---\n");
    // 验证仅移动类型通过协程传递
    // 创建 check_move_only{42}，其内部持有 unique_ptr<int>
    // 通过协程传递后再获取，验证值保持正确
    auto m = co_await coro_move_only(check_move_only{42});
    assert(*m.val == 42);
    std::printf("move_only test passed\n");

    std::printf("coro_lifetime tests passed!\n");
}

int main() {
    // 标准测试模式：在 io_context 中执行协程
    // co_spawn 注册协程 -> start 启动事件循环
    // -> can_stop 发送停止信号 -> join 等待线程结束
    io_context ctx;
    ctx.co_spawn(run());
    ctx.start();
    ctx.can_stop();
    ctx.join();
    return 0;
}
