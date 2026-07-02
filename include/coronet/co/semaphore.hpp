#pragma once

#include "coronet/config/io_context.hpp"
#include "coronet/detail/spinlock.hpp"
#include "coronet/detail/thread_meta.hpp"

#include <atomic>
#include <coroutine>
#include <type_traits>

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
/// ## 设计原理
/// 信号量维护两个原子变量：
///   - counter_ : 当前可用资源计数（可正可负，负值表示等待者数量）
///   - awaiting_: 等待者链表头（原子指针）
///
/// 当 counter_ > 0 时，acquire 直接递减并返回（无等待路径）。
/// 当 counter_ <= 0 时，acquire 将协程加入等待链表并挂起。
/// release 递增 counter_，若 counter_ 仍 <= 0 说明有等待者，
/// 则从等待链表唤醒一个协程。
///
/// ## 与 mutex 的对比
///   - mutex 是特殊化的二元信号量
///   - semaphore 支持任意计数（由 config::semaphore_counting_t 指定）
///   - semaphore 的等待链表操作逻辑类似，但多了一个计数维度
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

        /// 尝试获取资源。如果 counter_ > 0 则递减并返回 true（无需挂起），
        /// 否则返回 false（需要挂起等待）。
        ///
        /// Try to acquire. Returns true if no suspension needed.
        bool await_ready() noexcept {
            T old_counter = sem_.counter_.fetch_sub(1, std::memory_order_acquire);
            return old_counter > 0;
        }

        /// 挂起当前协程并将其加入信号量的等待链表。
        void await_suspend(std::coroutine_handle<> current) noexcept;
        void await_resume() const noexcept {}

    private:
        void co_spawn() const noexcept;

        counting_semaphore& sem_;
        acquire_awaiter* next_ = nullptr;       // 链表指针
        std::coroutine_handle<> handle_;         // 要恢复的协程句柄
        io_context* resume_ctx_;                 // 应在哪个 io_context 上恢复

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
    /// 尝试从等待队列中取出一个等待者。
    /// 内部会反转头插法的链表为 FIFO 顺序，再取头节点。
    ///
    /// Try to pop one waiter from the pending list.
    acquire_awaiter* try_release() noexcept;

    std::atomic<acquire_awaiter*> awaiting_;  // 等待链表头（无锁头插法）
    acquire_awaiter* to_resume_ = nullptr;     // 反转后的 FIFO 链表缓存
    std::atomic<T> counter_;                   // 当前信号量计数
    detail::spinlock notifier_mtx_;            // 保护 to_resume_ 链表的自旋锁
};

} // namespace coronet
