#pragma once

#include "coronet/co/mutex.hpp"
#include "coronet/detail/spinlock.hpp"
#include "coronet/detail/thread_meta.hpp"
#include "coronet/detail/trivial_task.hpp"

#include <atomic>
#include <coroutine>

namespace coronet {

class condition_variable;

namespace detail {

/// wait() 返回的 awaitable —— 结合了 mutex 解锁和条件等待。
///
/// ## 关键设计：unlock-ahead 模式
///
/// 传统的 condition_variable::wait 需要三个步骤：
///   1. 解锁 mutex（让生产者可以进入临界区修改条件）
///   2. 挂起等待条件成立
///   3. 被唤醒后重新锁定 mutex
///
/// 由于协程不能在挂起时持有锁（否则其他协程无法进入临界区），
/// cv_wait_awaiter 实现了一个"先解锁再挂起"的模式：
///
///   在 await_suspend 中，先将协程注册到条件变量的等待链表，
///   然后立即调用 mutex 的 unlock（通过 unlock_ahead）。
///
///   当被 notify_one 唤醒时，lock_awaken_handle_ 会重新尝试
///   获取 mutex 锁（通过 register_awaiting），获取锁后才真正恢复协程。
///
/// 这种设计避免了在协程挂起期间持有互斥锁的关键问题。
///
/// The awaitable returned by wait(). Implements "unlock-ahead" pattern:
/// unlock the mutex before suspending, re-acquire before resuming.
class [[nodiscard("Did you forget to co_await?")]] cv_wait_awaiter final {
public:
    using mutex = coronet::mutex;

    explicit cv_wait_awaiter(condition_variable& cv, mutex& mtx) noexcept
        : lock_awaken_handle_(mtx.lock())   // 预先创建一个 lock_awaiter
        , cv_(cv) {}

    static constexpr bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> current) noexcept;
    constexpr void await_resume() const noexcept {}

private:
    mutex::lock_awaiter lock_awaken_handle_;  // 用于唤醒后重新获取 mutex
    condition_variable& cv_;
    cv_wait_awaiter* next_ = nullptr;          // 等待链表指针

    friend class ::coronet::condition_variable;
};

} // namespace detail

/// 协程条件变量 —— 基于 mutex 和等待链表的异步条件等待。
///
/// Coroutine condition variable — async condition waiting with mutex.
///
/// ## 与 std::condition_variable 的对比
/// std::condition_variable 的 wait() 会阻塞线程，协程版本允许挂起。
/// 内部维护一个原子等待链表（awaiting_），notify_one/notify_all 从不阻塞。
///
/// ## 设计要点
///   - notifier_mtx_ 自旋锁串行化链表操作
///   - to_resume_head_ / to_resume_tail_ 缓存反转后的 FIFO 链表
///   - 支持带谓词的 wait 重载（条件满足时自动跳过等待）
///
/// @note 必须与 coronet::mutex 配合使用，不能与 std::mutex 搭配。
class condition_variable final {
public:
    using cv_wait_awaiter = detail::cv_wait_awaiter;

    condition_variable() noexcept = default;
    ~condition_variable() noexcept = default;

    /// 无条件等待——被唤醒后需重新检查条件。
    ///
    /// Wait without predicate — re-check condition after wake.
    cv_wait_awaiter wait(mutex& mtx) noexcept {
        return cv_wait_awaiter{*this, mtx};
    }

    /// 带谓词的等待——条件满足时不挂起，否则循环等待。
    ///
    /// Wait with predicate — skip if predicate is already true.
    /// 返回 trivial_task，使用 co_await 等待。
    template<std::predicate Pred>
    detail::trivial_task wait(mutex& mtx, Pred stop_waiting) {
        while (!stop_waiting()) {
            co_await this->wait(mtx);
        }
    }

    void notify_one() noexcept;
    void notify_all() noexcept;

private:
    friend class detail::cv_wait_awaiter;

    std::atomic<cv_wait_awaiter*> awaiting_{nullptr};  // 等待链表头（无锁头插法）
    cv_wait_awaiter* to_resume_head_ = nullptr;          // 反转后的 FIFO 链表头
    cv_wait_awaiter* to_resume_tail_ = nullptr;          // 反转后的 FIFO 链表尾
    detail::spinlock notifier_mtx_;                      // 保护链表操作的轻量自旋锁
};

} // namespace coronet
