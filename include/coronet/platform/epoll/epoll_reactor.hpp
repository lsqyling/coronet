#pragma once
// ============================================================
// epoll_reactor.hpp — epoll 反应器模式 Proactor 头文件
// ============================================================
// epoll 是 Linux  readiness-based I/O 模型：
//   内核通知 fd 就绪（可读/可写），用户态执行实际的 I/O syscall。
// 这与 io_uring 的 completion-based 模型不同（内核直接完成 I/O）。
//
// 关键设计：
//   - EPOLLONESHOT + EPOLLET：每次操作只触发一次，避免惊群。
//   - epoll_event.data.ptr 存储 epoll_completion_ctx*（含函数指针），
//     Proactor 通过函数指针调用具体 I/O，零虚表开销。
//   - data.ptr 与 data.fd 共用 union，因此 fd 独立存储在 ctx 中。
//   - eventfd 实现跨线程唤醒（与 io_uring 相同模式）。

#include "coronet/config/io_context.hpp"
#include "coronet/platform/platform.hpp"

#include <sys/epoll.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace coronet::platform::epoll {

// ============================================================
// epoll_completion_ctx — 每次 I/O 操作的完成上下文
// ============================================================
// 存储在 epoll_event.data.ptr 中。
// Proactor 在 epoll_wait 返回后，从中取出函数指针执行实际 I/O。
//
// Per-operation completion context (stored in epoll_event.data.ptr).
// The proactor reads this from a ready epoll_event and calls
// perform(fd, self) to execute the actual I/O syscall.

struct epoll_completion_ctx {
    // task_info 指针编码值（由 awaiter 构造时填充）
    // encoded task_info pointer (populated by awaiter ctor)
    uint64_t user_data;

    // 执行实际 I/O syscall 的函数指针（静态函数，零虚表开销）
    // performs the actual I/O syscall — static function, zero vtable cost
    int  (*perform)(int fd, void* self) noexcept;

    // 指向具体 awaiter 实例的指针（perform 内部 static_cast 回具体类型）
    // pointer to the concrete awaiter (cast back inside perform)
    void* self;

    // 注册到 epoll 的 fd。
    // 必须独立存储，因为 epoll_event.data 是 union，设置 data.ptr 会覆盖 data.fd。
    // The registered fd.  Must be stored separately because epoll_event.data
    // is a union — setting data.ptr clobbers data.fd.
    int  fd;
};

// ============================================================
// epoll_operation — 轻量操作包装（API 兼容性）
// ============================================================
// epoll 模型中，"操作" 由 epoll_completion_ctx + awaiter 承载。
// 此类仅用于满足 proactor_concept 的 operation_type 要求。
//
// Lightweight wrapper for API compatibility with the proactor concept.
// In epoll, the real "operation" is carried by epoll_completion_ctx.

class epoll_operation {
public:
    epoll_operation() noexcept = default;

    void set_user_data(uint64_t ud) noexcept { user_data_ = ud; }
    void prepare() noexcept {}  // no-op: I/O 参数存在 awaiter 中 / I/O params stored in awaiter
    void cancel() noexcept { cancelled_ = true; }

    uint64_t get_user_data() const noexcept { return user_data_; }
    bool is_cancelled() const noexcept { return cancelled_; }

private:
    uint64_t user_data_ = 0;
    bool cancelled_ = false;
};

// ============================================================
// epoll_proactor — 基于 epoll 的反应器（编译期多态，零虚表）
// ============================================================
// 与 io_uring_proactor / iocp_proactor 满足相同的隐式 proactor 概念。
// 核心差异：
//   - io_uring：submit() 批量提交 SQE，wait_completion() 收割 CQE
//   - epoll：   submit() 调用 epoll_wait 预取事件到内部队列，
//              wait_completion() 从队列取出事件 → 执行 I/O → 返回 completion_info
//
// Epoll-based reactor — satisfies the same implicit proactor concept
// as io_uring_proactor and iocp_proactor.  Compile-time selected, no virtual dispatch.

class epoll_proactor {
public:
    using operation_type = epoll_operation;

    epoll_proactor() noexcept = default;
    ~epoll_proactor() noexcept { deinit(); }

    // 不可拷贝、不可移动（独占 epoll fd 和 eventfd）
    // Non-copyable, non-movable (exclusive ownership of epoll fd + eventfd)
    epoll_proactor(const epoll_proactor&) = delete;
    epoll_proactor& operator=(const epoll_proactor&) = delete;
    epoll_proactor(epoll_proactor&&) = delete;
    epoll_proactor& operator=(epoll_proactor&&) = delete;

    // ---- 标准 Proactor 接口 / Standard proactor interface ----

    /// 初始化：创建 epoll fd + eventfd + 预分配事件缓冲区
    void init(uint32_t max_events);
    /// 清理：关闭 eventfd → 关闭 epfd
    void deinit() noexcept;

    /// 获取一个操作槽位（API 兼容，epoll 中几乎不用）
    std::unique_ptr<epoll_operation> acquire_operation();

    /// 提交：调用 epoll_wait 预取就绪事件到内部队列
    int  submit(bool wait = false) noexcept;
    /// 等待完成：从内部队列取出一个事件 → 执行 I/O syscall → 填充 completion_info
    int  wait_completion(completion_info* info) noexcept;
    /// 返回 epoll fd（用于外部集成）
    intptr_t native_handle() const noexcept { return epfd_; }

    /// 跨线程唤醒：往 eventfd 写入 1 字节，解除 wait_completion 阻塞
    /// Wake up a blocked wait_completion() via eventfd for cross-thread co_spawn.
    void wakeup() noexcept;

    /// 批量完成收割（用于批量处理场景）
    int poll_completions_impl(void* ctx,
        void (*callback_fn)(void*, const completion_info*)) noexcept;

    // ---- Epoll 专用接口（供 awaiter 调用）/ Epoll-specific (used by awaiters) ----

    /// 向 epoll 注册 fd（EPOLL_CTL_ADD，EPOLLONESHOT | EPOLLET）
    /// Register an fd with epoll (EPOLL_CTL_ADD with EPOLLONESHOT | EPOLLET).
    void register_fd(int fd, uint32_t events, epoll_completion_ctx* ctx) noexcept;

    /// 修改已有注册
    /// Modify an existing registration.
    void modify_fd(int fd, uint32_t events, epoll_completion_ctx* ctx) noexcept;

    /// 从 epoll 移除 fd
    /// Remove an fd from epoll.
    void unregister_fd(int fd) noexcept;

private:
    int epfd_ = -1;              // epoll 实例 fd / epoll instance
    int eventfd_ = -1;           // 跨线程唤醒 eventfd / eventfd for cross-thread wakeup
    uint32_t max_events_ = 0;    // 每次 epoll_wait 最大事件数
    bool initialized_ = false;

    // 预分配的就绪事件缓冲区 + 内部队列游标
    // Pre-allocated buffer + queue cursors for epoll_wait results
    std::vector<epoll_event> ready_events_;
    size_t ready_pos_ = 0;       // 当前消费位置 / current consume position
    size_t ready_count_ = 0;     // 有效事件数 / number of valid events

    // 将 eventfd 注册到 epoll（用于跨线程唤醒）
    void arm_eventfd();
    // 排空 eventfd 中的数据（唤醒后清理）
    void drain_eventfd() noexcept;

    /// 调用 epoll_wait 填充就绪队列。wait=true 时阻塞直到有事件。
    /// Call epoll_wait. Returns true if events are ready (or if eventfd fired).
    bool fill_ready_queue(bool wait) noexcept;
};

} // namespace coronet::platform::epoll
