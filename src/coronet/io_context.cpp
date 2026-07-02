// ============================================================
// io_context.cpp — 核心事件循环 / 协程调度器实现
// ============================================================
// 每个 io_context 运行一个专用线程，事件循环分为 4 个阶段：
//   0. drain_cross_thread() — 搬移跨线程队列的协程句柄到 SPSC 环
//   1. do_worker_part()     — 从 SPSC 环恢复就绪协程
//   2. do_submission_part() — 提交批量 I/O（仅 io_uring 需要）
//   3. do_completion_part() — 收割 I/O 完成事件
//
// Proactor 是 io_context 的栈上成员（具体类型，零虚表分派），
// 编译时由 CORONET_PLATFORM_WINDOWS / CORONET_USE_IOURING 决定。

#include "coronet/io_context.hpp"
#include "coronet/detail/worker_meta.hpp"
#include "coronet/detail/thread_meta.hpp"
#include "coronet/detail/io_context_meta.hpp"
#include "coronet/log/log.hpp"

#if defined(CORONET_PLATFORM_WINDOWS)
#include <winsock2.h>
#include <mutex>
#endif

#include <cstdio>
#include <cstdlib>

namespace coronet {

// ---- Windows Winsock 初始化 ----
// Windows 上使用 socket API 前必须先调用 WSAStartup。
// 使用 std::call_once 确保只初始化一次，无论创建了多少个 io_context。
#if defined(CORONET_PLATFORM_WINDOWS)
namespace {
    std::once_flag g_wsa_init_flag;
    // WSAStartup 初始化 Windows sockets 库，请求 2.2 版本
    // 失败时直接终止程序 —— 没有 Winsock 库无法进行任何网络 I/O
    void init_winsock() {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            std::fprintf(stderr, "FATAL: WSAStartup failed\n");
            std::abort();
        }
    }
}
#endif

// ---- io_context 构造/析构 ----

io_context::io_context() noexcept {
#if defined(CORONET_PLATFORM_WINDOWS)
    std::call_once(g_wsa_init_flag, init_winsock);
#endif
    // 从全局注册表分配唯一 ID。relaxed 顺序足够 —— 仅用作标识，不涉及数据同步。
    id_ = detail::g_io_context_meta.create_count.fetch_add(1,
        std::memory_order_relaxed);
    log::d("[io_context] constructor: id=%u\n", id_);

    // Proactor is a concrete stack-allocated member (no heap alloc, no virtual dispatch)
    // Proactor 是栈上具体类型成员，无需堆分配，无需虚函数分派。
    worker_.proactor = &proactor_;
    proactor_.init(config::default_io_uring_entries);
    worker_.ctx_id = id_;
    log::d("[io_context] constructor done\n");
}

// 析构时自动停止事件循环并等待线程退出
io_context::~io_context() noexcept {
    can_stop();
    join();
}

// 清理 Proactor 资源（由 run() 在事件循环退出后调用）
void io_context::deinit() noexcept {
    proactor_.deinit();
}

// ---- 生命周期管理 ----

// 启动事件循环线程。start() 非阻塞，立即返回。
void io_context::start() {
    log::d("[io_context] start() — spawning thread\n");
    // 标记自己已就绪（屏障的一部分）
    detail::g_io_context_meta.ready_count.fetch_add(1,
        std::memory_order_release);
    // 启动独立线程运行事件循环
    host_thread_ = std::thread(&io_context::run, this);
}

// 等待事件循环线程退出
void io_context::join() {
    if (host_thread_.joinable()) {
        host_thread_.join();
    }
}

// ---- 协程提交 ----

// 向本 io_context 提交一个协程任务（线程安全）。
// entrance 是 task<void>，通过 detach() 将协程句柄与 task 对象解耦，
// 然后通过 co_spawn_auto 将句柄送到调度队列。
void io_context::co_spawn(task<void>&& entrance) noexcept {
    auto handle = entrance.get_handle();
    log::d("[io_context] co_spawn: handle=%p\n", handle.address());
    entrance.detach();
    worker_.co_spawn_auto(handle);
}

// ---- 事件循环主函数 ----

void io_context::run() {
    log::i("[io_context] run() — thread started, id=%u\n", id_);

    // 设置线程局部存储，使此线程中的协程能 O(1) 访问当前上下文
    // Set thread-local
    detail::this_thread.ctx = this;
    detail::this_thread.worker = &worker_;
    detail::this_thread.ctx_id = id_;

    // 等待所有 io_context 都调用了 start()（全局屏障）
    // Wait for all io_contexts to be ready (barrier)
    detail::g_io_context_meta.wait_all_ready();
    log::d("[io_context] run() — barrier passed, entering main loop\n");

    // 事件循环：依次执行四个阶段
    uint64_t loop_count = 0;
    while (!will_stop_.load(std::memory_order_relaxed)) [[likely]] {
        loop_count++;
        if (loop_count <= 5 || loop_count % 1000 == 0) {
            log::v("[io_context] loop #%llu\n", (unsigned long long)loop_count);
        }
        // 阶段 0：从跨线程队列搬移协程句柄到 SPSC 环（必须先做，确保新任务能被调度到）
        worker_.drain_cross_thread();
        // 阶段 1：从 SPSC 环恢复所有就绪协程
        do_worker_part();
        // 阶段 2：提交批量 I/O 操作（仅 io_uring：submit SQEs；epoll/IOCP：no-op）
        do_submission_part();
        // 阶段 3：收割 I/O 完成事件
        do_completion_part();
    }

    log::i("[io_context] run() — loop exited after %llu iterations\n",
           (unsigned long long)loop_count);
    deinit();
    // 清理线程局部存储（将不再有效）
    detail::this_thread = {};
}

// ---- 四个事件循环阶段 ----

// 阶段 1：消耗 SPSC 环中所有就绪的协程句柄，逐个恢复执行
void io_context::do_worker_part() {
    while (auto handle = worker_.schedule()) {
        handle.resume();
    }
}

// 阶段 2：提交批量 I/O。仅 io_uring 需要显示提交（SQEs 在 awaiter 构造时已填入环，
// 需要 submit() 将它们发送到内核）。epoll 和 IOCP 的 I/O 在 await_suspend 中即时发起。
void io_context::do_submission_part() noexcept {
#if defined(CORONET_USE_IOURING)
    // io_uring: submit batched SQEs
    worker_.poll_submission();
#else
    // epoll / IOCP: no-op — operations are issued in await_suspend
    (void)0;
#endif
}

// 阶段 3：收割 I/O 完成事件。对 io_uring 是从 CQ ring 取 CQE，
// 对 epoll 是从就绪队列取事件后执行 I/O syscall，
// 对 IOCP 是从完成端口取 OVERLAPPED 结果。
void io_context::do_completion_part() noexcept {
    worker_.poll_completion();
}

// ---- 自由函数（方便的全局接口） ----

// 向"当前线程"的 io_context 提交协程任务
void co_spawn(task<void>&& entrance) noexcept {
    auto ctx = detail::this_thread.ctx;
    if (ctx) {
        ctx->co_spawn(std::move(entrance));
    }
}

// 获取当前线程的 io_context 引用
io_context& this_io_context() noexcept {
    return *detail::this_thread.ctx;
}

} // namespace coronet
