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
    // 自旋等待，直到所有已创建的 io_context 都调用了 start()
    // Spin-wait until all created io_contexts have called start()
    //
    // 自旋 vs 条件变量：
    // 在大多数使用场景中，io_context 数量很少（1-4 个），
    // 且 start() 在短时间内被连续调用，自旋等待的开销很小。
    // 条件变量虽然更优雅，但会引入额外的系统调用和唤醒延迟。
    //
    // In a proper implementation this would use a condition_variable,
    // but a simple spin is acceptable for the common case (1-4 contexts).
    while (ready_count.load(std::memory_order_acquire) <
           create_count.load(std::memory_order_acquire)) {
        // 让出 CPU 时间片，避免忙等耗尽 CPU
        // yield the CPU to avoid busy-wait starvation
        std::this_thread::yield();
    }
}

} // namespace coronet::detail
