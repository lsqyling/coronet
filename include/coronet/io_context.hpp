#pragma once
// ============================================================
// io_context.hpp — 核心事件循环 / 协程调度器
// ============================================================
// 每个 io_context 运行一个专用线程，单线程事件循环：
//   1. drain_cross_thread() — 从跨线程队列搬移协程句柄到 SPSC 环
//   2. do_worker_part()     — 从 SPSC 环恢复就绪协程
//   3. do_submission_part() — 提交批量 I/O（仅 io_uring；epoll/IOCP 为 no-op）
//   4. do_completion_part() — 收割 I/O 完成事件
//
// Proactor 编译期选择（零虚表开销）：
//   Windows       → iocp_proactor
//   Linux + IOURING → io_uring_proactor
//   Linux 默认     → epoll_proactor
//
// 用法 / Usage:
//   io_context ctx;
//   ctx.co_spawn(my_task());
//   ctx.start();
//   ctx.join();

#include "coronet/config/io_context.hpp"
#include "coronet/detail/worker_meta.hpp"
#include "coronet/task.hpp"

// 平台 Proactor：通过桥接头集中选择（零虚表分派）
// Platform proactor: selected via the proactor_selector bridge (no virtual dispatch)
#include "coronet/platform/proactor_selector.hpp"

#include <thread>
#include <atomic>

namespace coronet {

/// 核心事件循环 / 调度器 — 单线程，每个 io_context 一个线程。
/// Proactor 是栈上具体类型成员，零堆分配、零虚表。
///
/// The core event loop / scheduler — single-threaded, one per io_context.
/// No virtual dispatch, no heap allocation for the proactor.
class io_context final {
public:
    // 编译期平台 Proactor 类型（来自 proactor_selector 桥接）
    // Compile-time proactor type (from the proactor_selector bridge)
    using proactor_type = platform::proactor_type;

    io_context() noexcept;
    ~io_context() noexcept;

    // 不可拷贝、不可移动（独占线程 + Proactor 资源）
    // Non-copyable, non-movable
    io_context(const io_context&) = delete;
    io_context(io_context&&) = delete;
    io_context& operator=(const io_context&) = delete;
    io_context& operator=(io_context&&) = delete;

    // ---- 生命周期 / lifecycle ----

    /// 启动事件循环线程 / Start the event loop thread
    void start();
    /// 等待事件循环线程退出 / Wait for the event loop thread to exit
    void join();

    /// 请求优雅停止（线程安全）。设置停止标志 + 唤醒 Proactor。
    /// Request graceful stop (thread-safe).
    void can_stop() noexcept {
        will_stop_ = true;
        proactor_.wakeup();
    }

    /// 向本 io_context 提交一个协程任务（线程安全）
    /// Spawn a task onto this io_context (thread-safe).
    void co_spawn(task<void>&& entrance) noexcept;

    // ---- 访问器 / accessors ----

    /// io_context 唯一标识（0-254）
    config::ctx_id_t id() const noexcept { return id_; }

    /// 获取平台 Proactor 引用
    proactor_type& proactor() noexcept { return proactor_; }
    const proactor_type& proactor() const noexcept { return proactor_; }

    /// 提交原始协程句柄（供同步原语使用：mutex / sem / cv）
    /// Spawn a raw coroutine handle (used by synchronization primitives).
    void spawn_handle(std::coroutine_handle<> handle) noexcept {
        worker_.co_spawn_auto(handle);
    }

private:
    void deinit() noexcept;
    void run();                          // 事件循环主函数
    void drain_residual_coroutines();   // P1-5: 关闭时排空残留协程

    void do_worker_part();               // 从 SPSC 环恢复就绪协程
    void do_submission_part() noexcept;  // 提交批量 I/O（仅 io_uring）
    void do_completion_part() noexcept;  // 收割 I/O 完成事件

    // ---- 数据成员 / data ----

    // 具体 Proactor 类型，栈上分配，零虚表分派
    // concrete type, stack-allocated, zero virtual dispatch
    proactor_type proactor_;

    detail::worker_meta worker_;         // 调度器元数据
    std::thread host_thread_;            // 事件循环线程
    config::ctx_id_t id_;               // 上下文 ID
    bool started_ = false;              // P0-3: start() 是否被调用过
    // alignas: will_stop_ is written from other threads (can_stop()),
    // must not share a cache line with hot event-loop data (proactor_, worker_).
    // 缓存行隔离：will_stop_ 被其他线程写入（can_stop），不能与热数据共享缓存行。
    alignas(config::cache_line_size)
    std::atomic<bool> will_stop_{false}; // 停止标志
};

/// 自由函数：向当前线程的 io_context 提交任务
/// Free function: spawn a task on the current thread's io_context.
void co_spawn(task<void>&& entrance) noexcept;

/// 获取当前线程的 io_context 指针。
/// 返回 nullptr 表示当前线程不在 io_context 事件循环中。
///
/// Get the current thread's io_context pointer.
/// Returns nullptr if not running inside an io_context event loop.
io_context* this_io_context() noexcept;

} // namespace coronet
