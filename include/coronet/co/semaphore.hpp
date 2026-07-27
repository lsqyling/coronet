#pragma once

#include "coronet/config/io_context.hpp"
#include "coronet/detail/spinlock.hpp"
#include "coronet/detail/thread_meta.hpp"

#include <atomic>
#include <coroutine>

namespace coronet {

class io_context;

/// 协程安全的计数信号量 —— C++20 std::counting_semaphore 的协程版本。
///
/// Coroutine-safe counting semaphore — async version of std::counting_semaphore.
///
/// ## 为什么需要协程信号量？
/// 标准信号量的 acquire() 会阻塞线程，而协程版本允许线程在信号量不足时
/// 挂起等待，转而执行其他协程。这在异步 I/O 场景中极为关键：
/// 例如限制并发连接数、控制资源池访问等。
///
/// ## 设计原理（P1-2 修复后）
/// 信号量维护两个原子变量：
///   - counter_ : 当前可用资源计数（仅正值有意义，负值不会出现）
///   - awaiting_: 等待者链表头（原子指针）
///
/// 当 counter_ > 0 时，acquire 用 CAS 递减并返回（无等待路径）。
/// 当 counter_ <= 0 时，acquire 将协程加入等待链表并挂起。
///   插入链表后，重新检查 counter_ —— 如果在 await_ready 和 await_suspend
///   之间有 release 发生，counter_ 可能已 > 0，此时自唤醒（避免丢失唤醒）。
/// release 递增 counter_，然后总是尝试从等待链表唤醒一个协程。
///   若找到等待者，递减 counter_（资源转移给等待者）。
///
/// ## consumed_ 标志
/// 当 awaiter 通过 re-check 自唤醒时，设置 consumed_ = true。
/// release 的 try_release 会跳过 consumed 的 awaiter，防止双重唤醒。
/// 这在罕见竞态下发生（release 在 await_ready 和 await_suspend 之间执行）。
///
/// @note notifier_mtx_ 用于序列化 release 操作中的链表反转，
///       防止多个 release 并发操作 to_resume_ 链表导致数据竞争。
class counting_semaphore final {
private:
    using T = config::semaphore_counting_t;

    /// acquire() 的 awaitable，标记 [[nodiscard]] 防止遗漏 co_await。
    ///
    /// The awaitable for acquire(), [[nodiscard]] to prevent missing co_await.
    class [[nodiscard("Did you forget to co_await?")]] acquire_awaiter final {
    public:
        explicit acquire_awaiter(counting_semaphore& sem) noexcept
            : sem_(sem)
            , resume_ctx_(detail::this_thread.ctx) {}

        /// 尝试获取资源。仅当 counter_ > 0 时 CAS 递减。
        /// P1-2 fix: 不再无条件 fetch_sub（旧实现在 counter <= 0 时也递减，
        /// 导致 release 的 fast-path 误判有等待者，但 awaiter 尚未入链表）。
        ///
        /// Try to acquire. CAS-decrement only when counter > 0.
        bool await_ready() noexcept {
            T old = sem_.counter_.load(std::memory_order_acquire);
            while (old > 0) {
                if (sem_.counter_.compare_exchange_weak(
                        old, old - 1,
                        std::memory_order_acquire,
                        std::memory_order_relaxed)) {
                    return true;  // 获取成功
                }
            }
            return false;  // counter <= 0，需要挂起
        }

        /// 挂起当前协程并将其加入信号量的等待链表。
        /// P1-2 fix: 插入链表后 re-check counter，防止丢失唤醒。
        void await_suspend(std::coroutine_handle<> current) noexcept;
        void await_resume() const noexcept {}

    private:
        void co_spawn() const noexcept;

        counting_semaphore& sem_;
        acquire_awaiter* next_ = nullptr;       // 链表指针
        std::coroutine_handle<> handle_;         // 要恢复的协程句柄
        io_context* resume_ctx_;                 // 应在哪个 io_context 上恢复
        std::atomic<bool> consumed_{false};     // P1-2: 自唤醒标记，防止 release 双重唤醒

        friend class counting_semaphore;
    };

public:
    explicit counting_semaphore(T desired) noexcept
        : awaiting_(nullptr)
        , counter_(desired) {}

    counting_semaphore(const counting_semaphore&) = delete;
    ~counting_semaphore() noexcept;

    bool try_acquire() noexcept;
    acquire_awaiter acquire() noexcept { return acquire_awaiter{*this}; }

    void release() noexcept;
    void release(T update) noexcept;

private:
    /// 尝试从等待队列中取出一个非 consumed 的等待者。
    /// 跳过 consumed_ 标记的 awaiter（已通过 re-check 自唤醒）。
    ///
    /// Try to pop one non-consumed waiter from the pending list.
    acquire_awaiter* try_release() noexcept;

    std::atomic<acquire_awaiter*> awaiting_;  // 等待链表头（无锁头插法）
    acquire_awaiter* to_resume_ = nullptr;     // 反转后的 FIFO 链表缓存
    std::atomic<T> counter_;                   // 当前信号量计数
    detail::spinlock notifier_mtx_;            // 保护 to_resume_ 链表的自旋锁
};

} // namespace coronet
