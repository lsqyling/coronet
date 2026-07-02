// ============================================================
// thread_meta.cpp — 线程局部存储定义
// ============================================================
// 每个线程持有自己的 thread_meta 实例，提供 O(1) 访问当前
// io_context 和 worker_meta 的能力。
//
// 为什么用 thread_local 而非全局映射表：
//   - 零查找开销：读取 TLS 变量只需一次线性寻址，无锁无哈希
//   - 无竞争：每个线程独立读写，不存在伪共享问题
//   - 自然生命周期：线程退出时自动清理，无需手动注销
//   - 协程挂起/恢复时 TLS 保持不变（同线程执行）

#include "coronet/detail/thread_meta.hpp"

namespace coronet::detail {

// 每个线程的当前上下文信息
// 由 io_context::run() 在事件循环开始时设置
thread_local thread_meta this_thread;

} // namespace coronet::detail
