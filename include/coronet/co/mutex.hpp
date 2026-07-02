#pragma once

#include "coronet/detail/io_context_meta.hpp"
#include "coronet/detail/lock_guard.hpp"
#include "coronet/detail/thread_meta.hpp"

#include <atomic>
#include <cassert>
#include <coroutine>

namespace coronet::detail {
    class cv_wait_awaiter;
}

namespace coronet {

class io_context;

/// 协程互斥锁 —— 基于原子操作的无锁等待队列，非阻塞、可挂起。
///
/// Coroutine mutex — lock-free waiter list, non-blocking suspend.
///
/// ## 为什么需要协程互斥锁？
/// 标准库 std::mutex 会在 lock() 中阻塞线程，这在协程上下文中是灾难性的：
/// 阻塞意味着浪费一个线程的全部时间片，而该线程本可以调度其他协程。
/// coronet::mutex 允许协程在锁被持有时主动挂起（co_await），释放线程执行其他协程。
///
/// ## 设计原理
/// mutex 维护一个原子状态变量 awaiting_，该变量有三类取值：
///   - not_locked (1)        : 锁空闲
///   - locked_no_awaiting (0): 锁被持有，且没有等待者
///   - 其他值               : 锁被持有，该值是一个指向 lock_awaiter 链表头的指针
///
/// 这种设计将"锁状态"和"等待队列头指针"合二为一，避免了单独维护一个队列指针的原子性问题。
/// 链表节点（lock_awaiter）通常分配在协程栈帧上（即协程状态的一部分），零动态分配。
///
/// @note 等待者链表是反序插入的（头插法），unlock 时会反转链表再逐个唤醒，
///       以保证顺序公平性 —— 先等待的协程优先获取锁。
///
/// @warning 不能在 std::mutex 已锁定的作用域内 co_await 本锁，会造成死锁。
class mutex final {
public:
    /// lock() 的 awaitable 对象，标记 [[nodiscard]] 防止遗漏 co_await。
    ///
    /// The awaitable returned by lock(), [[nodiscard]] to prevent missing co_await.
    class [[nodiscard("Did you forget to co_await?")]] lock_awaiter {
    public:
        explicit lock_awaiter(mutex& mtx) noexcept
            : mtx_(mtx)
            , resume_ctx_(detail::this_thread.ctx)
        {
            assert(resume_ctx_ != nullptr && "locking mutex without an io_context");
        }

        // 永不就绪：总是需要尝试注册到等待队列
        static constexpr bool await_ready() noexcept { return false; }

        /// 尝试挂起 —— 如果成功获取锁（无竞争）则返回 false 不挂起。
        /// 如果失败则将当前协程加入等待队列，返回 true 进入挂起状态。
        ///
        /// Try to suspend — returns false if lock acquired immediately.
        bool await_suspend(std::coroutine_handle<> current) noexcept {
            awaken_coro_ = current;
            return register_awaiting();
        }

        void await_resume() const noexcept {}

    protected:
        void register_coroutine(std::coroutine_handle<> handle) noexcept {
            awaken_coro_ = handle;
        }

        std::coroutine_handle<> get_coroutine() noexcept { return awaken_coro_; }

        // 注册到等待队列。返回 true 表示挂起，false 表示立即获取锁。
        bool register_awaiting() noexcept;
        // 提前解锁（condition_variable 使用）：释放 mutex 再挂起等待条件
        void unlock_ahead() noexcept { mtx_.unlock(); }
        // 将协程投递到目标 io_context 恢复执行，而非直接 resume。
        // 这使得协程在正确的调度器上下文中恢复，保证公平调度。
        void co_spawn() const noexcept;

        mutex& mtx_;
        lock_awaiter* next_ = nullptr;          // 链表指针，指向等待队列中的下一个节点
        std::coroutine_handle<> awaken_coro_;    // 要恢复的协程句柄
        io_context* resume_ctx_;                 // 应在哪个 io_context 上恢复

        friend class mutex;
        friend class condition_variable;
        friend class detail::cv_wait_awaiter;
    };

    /// lock_guard() 的 awaitable 对象 —— 比 lock() 多返回一个 RAII 守卫。
    ///
    /// The awaitable for lock_guard(), returns a RAII guard on acquisition.
    class [[nodiscard("Did you forget to co_await?")]] lock_guard_awaiter final
        : public lock_awaiter {
    public:
        using lock_awaiter::lock_awaiter;
        using lock_guard = detail::lock_guard<mutex>;

        [[nodiscard]] lock_guard await_resume() const noexcept {
            return lock_guard{mtx_};
        }
    };

    mutex() noexcept : awaiting_(not_locked) {}
    ~mutex() noexcept;

    bool try_lock() noexcept;
    lock_awaiter lock() noexcept { return lock_awaiter{*this}; }
    lock_guard_awaiter lock_guard() noexcept { return lock_guard_awaiter{*this}; }
    void unlock() noexcept;

private:
    // 特殊状态值：
    //   locked_no_awaiting = 0   — 锁被持有，无等待者
    //   not_locked = 1           — 锁空闲
    //   其他值                    — 锁被持有，值为 lock_awaiter 链表头指针
    static constexpr uintptr_t locked_no_awaiting = 0;
    static constexpr uintptr_t not_locked = 1;

    std::atomic<uintptr_t> awaiting_;   // 锁状态 + 等待队列头（原子合并）
    lock_awaiter* to_resume_ = nullptr; // unlock 反向后的链表头，待逐个恢复
};

} // namespace coronet
