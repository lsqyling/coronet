// ============================================================
// io_context_meta.cpp — 全局 io_context 注册表 + 启动屏障
// ============================================================
// 提供多 io_context 场景下的同步机制：
//   所有 io_context 必须全部调用 start() 后才能开始事件循环。
//   这是通过 "create_count / ready_count 计数相等" 的屏障实现的。
//
// 为什么需要这个屏障：
//   - 跨 io_context 的 co_spawn 依赖目标上下文已运行
//   - 如果 io_context A 在 B 尚未 start 时向 B 发任务，会丢失唤醒信号
//   - 屏障保证所有上下文同时开始运行，消除竞态窗口

#include "coronet/detail/io_context_meta.hpp"

#include <thread>

namespace coronet::detail {

// 全局唯一的 io_context 注册表实例
io_context_meta g_io_context_meta;

void io_context_meta::wait_all_ready() noexcept {
    // C++20 std::atomic::wait/notify_all: uses OS futex (Linux) /
    // WaitOnAddress (Windows) — more efficient than spin-yield.
    // The wait blocks the thread until ready_count changes, avoiding
    // wasted CPU cycles. notify_all() is called in io_context::start().
    //
    // C++20 原子等待/通知：使用 OS futex（Linux）/ WaitOnAddress（Windows），
    // 比自旋 yield 更高效。线程阻塞直到 ready_count 变化，避免浪费 CPU。
    uint32_t cur = ready_count.load(std::memory_order_acquire);
    while (cur < create_count.load(std::memory_order_acquire)) {
        ready_count.wait(cur, std::memory_order_acquire);
        cur = ready_count.load(std::memory_order_acquire);
    }
}

} // namespace coronet::detail
