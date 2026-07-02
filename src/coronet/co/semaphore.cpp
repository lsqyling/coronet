#include "coronet/co/semaphore.hpp"
#include "coronet/io_context.hpp"

#include <cassert>

namespace coronet {

counting_semaphore::~counting_semaphore() noexcept {
    // 为简化设计，析构时不检查等待者泄露。
    // 实际使用中应确保所有 acquire 操作已完成（或取消）再销毁信号量。
}

bool counting_semaphore::try_acquire() noexcept {
    // 无等待的获取操作：仅当 counter_ > 0 时才尝试 CAS 递减。
    // 这是轻量级路径，不涉及任何链表操作。
    T old_counter = counter_.load(std::memory_order_relaxed);
    return old_counter > 0
           && counter_.compare_exchange_strong(
               old_counter, old_counter - 1, std::memory_order_acquire,
               std::memory_order_relaxed);
}

/// 释放单个资源，唤醒一个等待者（如有）。
///
/// ## 算法详解
///
/// 1. 先原子递增 counter_。如果递增前 counter_ >= 0，
///    说明没有协程在等待，直接返回（快速路径）。
///
/// 2. 如果递增前 counter_ < 0，说明有 |counter_| 个协程在等待，
///    需要唤醒一个。进入慢速路径：
///    a. 获取 notifier_mtx_ 自旋锁
///    b. 调用 try_release() 从等待队列取出一个 acquire_awaiter
///    c. 释放自旋锁
///    d. 通过 co_spawn 投递到目标 io_context 恢复执行
///
/// 自旋锁的必要性：release 可能被多个线程并发调用，
/// 必须串行化对 to_resume_ 链表的访问，防止数据竞争。
void counting_semaphore::release() noexcept {
    const T update = 1;
    const T old_counter = counter_.fetch_add(update, std::memory_order_release);
    if (old_counter >= 0) return;

    notifier_mtx_.lock();
    acquire_awaiter* awaken_awaiter = try_release();
    notifier_mtx_.unlock();

    if (awaken_awaiter) awaken_awaiter->co_spawn();
}

/// 批量释放 update 个资源，并唤醒对应的等待者。
///
/// 与单释放类似，但需要循环唤醒足够数量的等待者。
/// 使用 old_counter 和 update 计算需要实际唤醒的数量：
/// 如果 old_counter = -2 且 update = 5，则实际只需唤醒 2 个（因为 5 - 2 = 3 个空闲）。
///
/// Release `update` units and wake corresponding waiters.
void counting_semaphore::release(T update) noexcept {
    const T old_counter = counter_.fetch_add(update, std::memory_order_release);
    if (old_counter >= 0) return;

    // 计算需要实际唤醒的等待者数（负值表示还差多少个才到 0）
    update = std::max(old_counter, -update);
    {
        notifier_mtx_.lock();
        do {
            acquire_awaiter* awaken_awaiter = try_release();
            if (awaken_awaiter) awaken_awaiter->co_spawn();
        } while (++update < 0);
        notifier_mtx_.unlock();
    }
}

/// 从等待链表中取出一个等待者。
///
/// 与 mutex 的 unlock 类似：先检查 to_resume_ 缓存，
/// 如果为空则从原子 awaiting_ 中取出整个链表，反转顺序（恢复 FIFO），
/// 存入 to_resume_，再取出头节点返回。
///
/// ## 为什么要反转链表？
/// acquire_awaiter 使用头插法（无锁）插入等待链表，所以链表顺序是 LIFO。
/// 反转后变为 FIFO，保证先等待的协程先被唤醒（公平性）。
///
/// Try to pop one waiter from the linked list. Reverses the list
/// from LIFO (lock-free insertion order) to FIFO for fairness.
counting_semaphore::acquire_awaiter*
counting_semaphore::try_release() noexcept {
    acquire_awaiter* resume_head = to_resume_;
    if (resume_head == nullptr) [[unlikely]] {
        auto* node = awaiting_.exchange(nullptr, std::memory_order_acquire);
        if (node == nullptr) [[unlikely]]
            return nullptr;

        // 反转链表：头插法得到的是 LIFO 顺序，反转后为 FIFO
        do {
            acquire_awaiter* tmp = node->next_;
            node->next_ = resume_head;
            resume_head = node;
            node = tmp;
        } while (node != nullptr);
    }

    assert(resume_head != nullptr);
    // 取头节点返回，其余缓存到 to_resume_
    to_resume_ = resume_head->next_;
    return resume_head;
}

/// 将当前协程句柄存入 acquire_awaiter 并通过无锁头插法加入等待链表。
///
/// 无锁插入步骤：
/// 1. 读取当前链表头（awaiting_）
/// 2. 将 this->next_ 指向旧链表头
/// 3. CAS 尝试将 awaiting_ 更新为 this
/// 4. 如果 CAS 失败，重试（说明其他协程同时插入了新节点）
///
/// 这是典型的 lock-free stack push 操作。
void counting_semaphore::acquire_awaiter::await_suspend(
    std::coroutine_handle<> current) noexcept
{
    this->handle_ = current;
    acquire_awaiter* old_head = sem_.awaiting_.load(std::memory_order_relaxed);
    do {
        this->next_ = old_head;
    } while (!sem_.awaiting_.compare_exchange_weak(
        old_head, this, std::memory_order_release, std::memory_order_relaxed));
}

/// 通过 io_context 恢复协程执行。
///
/// 使用 co_spawn 而非直接 resume 的理由与 mutex 一致：
///   - 确保协程在其原始 io_context 上恢复
///   - 延迟到事件循环的下一次迭代，防止递归调用栈过深
void counting_semaphore::acquire_awaiter::co_spawn() const noexcept {
    this->resume_ctx_->spawn_handle(this->handle_);
}

} // namespace coronet
