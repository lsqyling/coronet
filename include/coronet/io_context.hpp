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
#include "coronet/detail/thread_meta.hpp"
#include "coronet/detail/io_context_meta.hpp"
#include "coronet/task.hpp"

// 平台特定 Proactor：编译期选择（零虚表分派）
// Platform-specific proactor: compile-time selection (no virtual dispatch)
#if defined(CORONET_PLATFORM_WINDOWS)
#include "coronet/platform/iocp/iocp_proactor.hpp"
#elif defined(CORONET_USE_IOURING)
#include "coronet/platform/io_uring/io_uring_proactor.hpp"
#else
#include "coronet/platform/epoll/epoll_reactor.hpp"
#endif

#include <thread>
#include <atomic>

namespace coronet {

/// 核心事件循环 / 调度器 — 单线程，每个 io_context 一个线程。
/// Proactor 是栈上具体类型成员，零堆分配、零虚表。
///
/// The core event loop / scheduler — single-threaded, one per io_context.
/// No virtual dispatch, no heap allocation for the proactor.
class [[nodiscard]] io_context final {
public:
    // 编译期平台 Proactor 类型选择 / Compile-time proactor type selection
#if defined(CORONET_PLATFORM_WINDOWS)
    using proactor_type = platform::iocp::iocp_proactor;
#elif defined(CORONET_USE_IOURING)
    using proactor_type = platform::io_uring::io_uring_proactor;
#else
    using proactor_type = platform::epoll::epoll_proactor;
#endif

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
    std::atomic<bool> will_stop_{false}; // 停止标志
};

/// 自由函数：向当前线程的 io_context 提交任务
/// Free function: spawn a task on the current thread's io_context.
void co_spawn(task<void>&& entrance) noexcept;

/// 获取当前线程的 io_context 引用
/// Get the current thread's io_context.
io_context& this_io_context() noexcept;

} // namespace coronet
