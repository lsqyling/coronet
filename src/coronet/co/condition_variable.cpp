#include "coronet/co/condition_variable.hpp"
#include "coronet/io_context.hpp"

#include <utility>

namespace coronet::detail {

/// 挂起当前协程等待条件变量通知。
///
/// ## 操作流程
///
/// 1. 将协程句柄注册到 lock_awaken_handle_（该句柄稍后用于重新获取 mutex）
/// 2. 通过无锁头插法将当前 cv_wait_awaiter 加入条件变量的等待链表
/// 3. 先解锁 mutex（unlock_ahead）：使得生产者可以进入临界区修改条件
///
/// 第三步至关重要：如果不在挂起前解锁 mutex，其他协程将永远无法
/// 进入临界区修改条件，导致死锁。
///
/// 唤醒顺序：notify_one -> 重新获取 mutex lock -> resume 协程。
void cv_wait_awaiter::await_suspend(std::coroutine_handle<> current) noexcept {
    this->lock_awaken_handle_.register_coroutine(current);

    // 将当前 awaiter 加入条件变量的等待链表（无锁头插法）
    cv_wait_awaiter* old_head = cv_.awaiting_.load(std::memory_order_relaxed);
    do {
        this->next_ = old_head;
    } while (!cv_.awaiting_.compare_exchange_weak(
        old_head, this, std::memory_order_release, std::memory_order_relaxed));

    // 解锁 mutex：允许生产者进入临界区
    // 此时协程已注册到条件变量链表，被唤醒时会自动重新获取锁
    this->lock_awaken_handle_.unlock_ahead();
}

} // namespace coronet::detail

namespace coronet {

/// 唤醒一个等待协程。
///
/// ## 算法详解
///
/// 1. 先检查 to_resume_head_ 缓存，如果不为空直接取出头节点唤醒。
///
/// 2. 如果缓存为空，从原子 awaiting_ 中取出整个链表。
///    由于头插法，取出的链表是 LIFO 顺序，需要反转成 FIFO。
///    反转时记录头尾节点，便于后续追加。
///
/// 3. 对取出的第一个节点：
///    a. 调用 lock_awaken_handle_.register_awaiting() 尝试获取 mutex
///    b. 如果成功（返回 false 表示立即获取到锁），直接 co_spawn 恢复
///    c. 如果失败（返回 true 表示需要等待 mutex），不做额外操作，
///       register_awaiting 已将其加入 mutex 的等待队列
///
/// 自旋锁 notifier_mtx_ 用于防止多个 notify 并发操作缓存链表。
///
/// Wake one waiting coroutine.
void condition_variable::notify_one() noexcept {
    auto try_notify_one = [](cv_wait_awaiter* head) {
        auto& awaken_awaiter = head->lock_awaken_handle_;
        // 尝试获取 mutex：如果成功立即恢复，失败说明已在 mutex 等待队列中
        if (!awaken_awaiter.register_awaiting()) [[unlikely]] {
            awaken_awaiter.co_spawn();
        }
    };

    cv_wait_awaiter* head;

    notifier_mtx_.lock();
    head = to_resume_head_;
    if (to_resume_head_ != nullptr) {
        // 从 FIFO 缓存中取头节点
        to_resume_head_ = to_resume_head_->next_;
        notifier_mtx_.unlock();
        try_notify_one(head);
        return;
    }
    to_resume_tail_ = nullptr;
    notifier_mtx_.unlock();

    // 从原子等待队列取出整个链表
    head = awaiting_.exchange(nullptr, std::memory_order_acquire);
    if (head == nullptr) [[unlikely]]
        return;

    // 反转链表 LIFO -> FIFO
    cv_wait_awaiter* const tail = head;
    cv_wait_awaiter* succ = nullptr;
    cv_wait_awaiter* pred = head->next_;
    while (pred != nullptr) {
        head->next_ = succ;
        succ = head;
        head = pred;
        pred = pred->next_;
    }

    // 将反转后的链表（除第一个节点外）缓存起来
    notifier_mtx_.lock();
    if (to_resume_head_ == nullptr) [[likely]] {
        // 缓存为空：直接存入
        to_resume_head_ = succ;
        to_resume_tail_ = tail;
        notifier_mtx_.unlock();
        try_notify_one(head);
        return;
    }
    // 缓存非空：追加到尾部
    to_resume_tail_->next_ = head;
    cv_wait_awaiter* const to_notify = to_resume_head_;
    to_resume_head_ = to_resume_head_->next_;
    to_resume_tail_ = tail;
    notifier_mtx_.unlock();
    try_notify_one(to_notify);
}

/// 唤醒所有等待协程。
///
/// 与 notify_one 类似，但一次性从 awaiting_ 和 to_resume_head_ 中
/// 取出所有等待者，逐一尝试获取 mutex 后恢复。
///
/// Wake all waiting coroutines.
void condition_variable::notify_all() noexcept {
    auto try_notify_all = [](cv_wait_awaiter* head) {
        while (head != nullptr) {
            auto& awaken_awaiter = head->lock_awaken_handle_;
            if (!awaken_awaiter.register_awaiting()) [[unlikely]] {
                awaken_awaiter.co_spawn();
            }
            head = head->next_;
        }
    };

    // 先处理原子等待队列中的所有节点
    cv_wait_awaiter* resume_head =
        awaiting_.exchange(nullptr, std::memory_order_acquire);
    try_notify_all(resume_head);

    // 再处理缓存链表中的所有节点
    notifier_mtx_.lock();
    resume_head = to_resume_head_;
    to_resume_head_ = nullptr;
    notifier_mtx_.unlock();
    try_notify_all(resume_head);
}

} // namespace coronet
