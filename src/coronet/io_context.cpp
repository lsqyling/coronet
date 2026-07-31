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
            log::e("FATAL: WSAStartup failed\n");
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

// 析构时自动停止事件循环并等待线程退出。
// P0-3: 递减 create_count 和 ready_count，防止阶段性创建/销毁 io_context 时屏障死锁。
// create_count 在构造时递增，ready_count 在 start() 时递增。
// 两者都必须在析构时递减，否则：
//   - create_count 不递减 → 阶段性创建死锁
//   - ready_count 不递减 → 屏障过早通过（stale ready_count 使新 io_context 跳过等待）
io_context::~io_context() noexcept {
    can_stop();
    join();
    // 仅当 start() 曾被调用时才递减 ready_count
    if (started_) {
        detail::g_io_context_meta.ready_count.fetch_sub(
            1, std::memory_order_release);
    }
    // 递减 create_count
    auto old = detail::g_io_context_meta.create_count.fetch_sub(
        1, std::memory_order_release);
    // 递减后如果 create_count <= ready_count，屏障条件已满足，
    // 唤醒所有在 wait_all_ready 中阻塞的线程。
    if (old - 1 <= detail::g_io_context_meta.ready_count.load(
            std::memory_order_acquire)) {
        detail::g_io_context_meta.ready_count.notify_all();
    }
}

// 清理 Proactor 资源（由 run() 在事件循环退出后调用）
void io_context::deinit() noexcept {
    proactor_.deinit();
}

// ---- 生命周期管理 ----

// 启动事件循环线程。start() 非阻塞，立即返回。
void io_context::start() {
    log::d("[io_context] start() — spawning thread\n");
    started_ = true;
    // 标记自己已就绪（屏障的一部分）
    detail::g_io_context_meta.ready_count.fetch_add(1,
        std::memory_order_release);
    // C++20: notify any thread waiting in wait_all_ready()
    detail::g_io_context_meta.ready_count.notify_all();
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

    // io_uring: 在事件循环线程延迟初始化 ring，遵守 SINGLE_ISSUER 约束
    // io_uring: defer ring init to event loop thread (SINGLE_ISSUER compliance)
#if defined(CORONET_USE_IOURING)
    proactor_.lazy_init_ring();
#endif

    // 事件循环：依次执行四个阶段
    uint64_t loop_count = 0;
    // P1-1: acquire 确保与 can_stop() 中的 store(seq_cst) 建立 happens-before。
    // x86 上 acquire load 编译为普通 mov（零开销），ARM 上加 dmb ishld（可忽略）。
    while (!will_stop_.load(std::memory_order_acquire)) [[likely]] {
        loop_count++;
        // P2-1: 用 & 1023 替代 % 1000（位运算零开销），并用 if constexpr
        // 在编译期消除日志（生产环境 level=warning 时整个分支被删除）。
        if constexpr (config::level <= config::log_level::verbose) {
            if (loop_count <= 5 || (loop_count & 1023) == 0) {
                log::v("[io_context] loop #%llu\n", (unsigned long long)loop_count);
            }
        }
        // 阶段 0：从跨线程队列搬移协程句柄到 SPSC 环（必须先做，确保新任务能被调度到）
        worker_.drain_cross_thread();
        // 阶段 0.5：搬移延迟任务队列到 SPSC 环。
        // epoll 后端的 yield() 使用 defer_task() 将协程加入延迟队列以避免活锁，
        // 必须在 do_worker_part() 之前排空，否则 yield() 的协程永远不会恢复。
        // Drain deferred task queue into SPSC ring (epoll yield uses defer_task).
        worker_.drain_deferred();
        // 阶段 1：从 SPSC 环恢复所有就绪协程
        do_worker_part();
        // 阶段 2：提交批量 I/O 操作（仅 io_uring：submit SQEs；epoll/IOCP：no-op）
        do_submission_part();
        // 阶段 3：收割 I/O 完成事件
        do_completion_part();
    }

    log::i("[io_context] run() — loop exited after %llu iterations\n",
           (unsigned long long)loop_count);

    // P1-5: Drain phase — 恢复所有残留协程，避免帧泄漏。
    // SPSC 环和 cross_queue 中残留的 detached 协程如果不恢复，其协程帧永远不会销毁。
    // 非 detached 协程的父协程 co_await 会永远不返回 → 死锁。
    // drain 阶段恢复它们，让协程有机会清理资源或快速失败。
    drain_residual_coroutines();

    deinit();
    // 清理线程局部存储（将不再有效）
    detail::this_thread = {};
}

// ---- 四个事件循环阶段 ----

// P1-5: 关闭时排空残留协程。
// 事件循环退出后，SPSC 环和 cross_queue 中可能还有未恢复的协程句柄。
// 对于 detached task<void>，不恢复会导致协程帧泄漏。
// 对于非 detached task，父协程的 co_await 永远不返回 → 死锁。
// 此方法在 deinit() 前执行有限轮次的排空，恢复所有残留协程。
// 恢复的协程如果发起新 I/O，可能因 proactor 即将关闭而失败（通过异常或错误码处理）。
void io_context::drain_residual_coroutines() {
    // 首先排空跨线程队列和延迟队列
    worker_.drain_cross_thread();
    worker_.drain_deferred();
    worker_.work_once();  // 恢复一个就绪协程（触发级联恢复）

    // 有限轮次排空：防止恶意协程无限生成新任务导致无限循环
    // 每轮 drain_cross_thread + drain_deferred + do_worker_part，最多 3 轮
    for (int round = 0; round < 3 && worker_.has_task_ready(); ++round) {
        worker_.drain_cross_thread();
        worker_.drain_deferred();
        do_worker_part();
    }

    // 最终检查：如果仍有残留（理论上不应该），记录警告
    if (worker_.has_task_ready()) {
        log::w("[io_context] residual coroutines remain after drain phase\n");
    }
}

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
//
// io_uring 路径：每次循环收割一个 CQE。仅在 will_stop_ 未设置时
// 调用 poll_completion（其中 wait_completion 可能阻塞）。
// 如果 will_stop_ 已设置（由 can_stop() 在同一线程设置），
// 跳过阻塞等待，避免 TOCTOU 竞态导致的事件循环死锁：
//   can_stop() 设置 will_stop_ → 但之前的 eventfd CQE 已被消费并 re-arm
//   → 如果进入 poll_completion 的阻塞路径，没有新的唤醒者 → 死锁。
void io_context::do_completion_part() noexcept {
#if defined(CORONET_USE_IOURING)
    // P1-3: 仅在未停止时等待完成事件。
    // 如果 will_stop_ 已设置，说明 can_stop() 已在此迭代的 do_worker_part
    // 中被调用，跳过阻塞以避免等待永远不会到来的 CQE。
    if (!will_stop_.load(std::memory_order_acquire)) {
        worker_.poll_completion();
    }
#else
    worker_.poll_completion();
#endif
}

// ---- 自由函数（方便的全局接口） ----

// 向"当前线程"的 io_context 提交协程任务。
// P1-4: 如果在非 io_context 线程调用（ctx == null），记录警告而非静默丢弃。
// entrance 析构时会安全销毁协程帧。
void co_spawn(task<void>&& entrance) noexcept {
    auto ctx = detail::this_thread.ctx;
    if (ctx) [[likely]] {
        ctx->co_spawn(std::move(entrance));
    } else {
        log::w("[co_spawn] called from non-io_context thread, task dropped\n");
        // entrance 析构时安全销毁协程帧
    }
}

// 获取当前线程的 io_context 指针。
// 返回 nullptr 表示当前线程不在 io_context 事件循环中（如 main 线程）。
// 旧版本返回引用并解引用可能为 null 的指针 → UB。
io_context* this_io_context() noexcept {
    return detail::this_thread.ctx;
}

} // namespace coronet
