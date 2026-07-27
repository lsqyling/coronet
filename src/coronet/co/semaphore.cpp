#include "coronet/co/semaphore.hpp"
#include "coronet/io_context.hpp"

#include <cassert>
#include <algorithm>

namespace coronet {

counting_semaphore::~counting_semaphore() noexcept {
    // 为简化设计，析构时不检查等待者泄露。
    // 实际使用中应确保所有 acquire 操作已完成（或取消）再销毁信号量。
}

bool counting_semaphore::try_acquire() noexcept {
    // 无等待的获取操作：仅当 counter_ > 0 时才尝试 CAS 递减。
    T old_counter = counter_.load(std::memory_order_relaxed);
    return old_counter > 0
           && counter_.compare_exchange_strong(
               old_counter, old_counter - 1, std::memory_order_acquire,
               std::memory_order_relaxed);
}

/// 释放单个资源，唤醒一个等待者（如有）。
///
/// P1-2 fix: 不再依赖 counter_ 的符号判断是否有等待者。
/// 总是尝试从等待链表取出一个等待者。若找到：
///   - fetch_sub 递减 counter_（资源转移给等待者）
///   - co_spawn 恢复等待者协程
/// 若未找到等待者，counter_ 保持递增后的值（资源留给下次 acquire）。
///
/// Release one unit, wake a waiter if any.
/// Always tries to pop from the wait list (not just when counter < 0).
void counting_semaphore::release() noexcept {
    counter_.fetch_add(1, std::memory_order_release);

    notifier_mtx_.lock();
    acquire_awaiter* awaken_awaiter = try_release();
    notifier_mtx_.unlock();

    if (awaken_awaiter) {
        // 资源转移给等待者：递减 counter_
        counter_.fetch_sub(1, std::memory_order_acq_rel);
        awaken_awaiter->co_spawn();
    }
}

/// 批量释放 update 个资源，并唤醒对应的等待者。
///
/// P1-2 fix: 与单释放一致，总是尝试从链表取出等待者。
/// 每找到一个等待者，递减 counter_（资源转移）。
///
/// Release `update` units and wake corresponding waiters.
void counting_semaphore::release(T update) noexcept {
    counter_.fetch_add(update, std::memory_order_release);

    notifier_mtx_.lock();
    for (T i = 0; i < update; ++i) {
        acquire_awaiter* awaken_awaiter = try_release();
        if (awaken_awaiter) {
            // 在锁内取句柄，锁外 co_spawn（减少临界区）
            notifier_mtx_.unlock();
            counter_.fetch_sub(1, std::memory_order_acq_rel);
            awaken_awaiter->co_spawn();
            notifier_mtx_.lock();
        } else {
            break;  // 没有更多等待者
        }
    }
    notifier_mtx_.unlock();
}

/// 从等待链表中取出一个非 consumed 的等待者。
///
/// P1-2 fix: 跳过 consumed_ 标记的 awaiter。
/// consumed_ 在 await_suspend 的 re-check 路径中设置（自唤醒时），
/// 表示该 awaiter 已通过 re-check 自行获取资源，不应被 release 再次唤醒。
///
/// Try to pop one non-consumed waiter. Skips awaiters that have
/// already self-woken via the re-check in await_suspend.
counting_semaphore::acquire_awaiter*
counting_semaphore::try_release() noexcept {
    acquire_awaiter* resume_head = to_resume_;

    // 跳过缓存中已 consumed 的节点
    while (resume_head && resume_head->consumed_.load(std::memory_order_acquire)) {
        resume_head = resume_head->next_;
    }

    if (resume_head == nullptr) [[unlikely]] {
        // 缓存为空，从原子链表取出全部
        auto* node = awaiting_.exchange(nullptr, std::memory_order_acquire);
        if (node == nullptr) [[unlikely]]
            return nullptr;

        // 反转链表（LIFO → FIFO），跳过 consumed 节点
        do {
            acquire_awaiter* tmp = node->next_;
            if (!node->consumed_.load(std::memory_order_acquire)) {
                node->next_ = resume_head;
                resume_head = node;
            }
            node = tmp;
        } while (node != nullptr);
    }

    if (resume_head == nullptr) [[unlikely]]
        return nullptr;

    // 跳过头部的 consumed 节点（可能在反转过程中被标记）
    while (resume_head && resume_head->consumed_.load(std::memory_order_acquire)) {
        resume_head = resume_head->next_;
    }
    if (resume_head == nullptr) {
        to_resume_ = nullptr;
        return nullptr;
    }

    // 取头节点返回，剩余缓存到 to_resume_
    to_resume_ = resume_head->next_;
    return resume_head;
}

/// 将当前协程句柄存入 acquire_awaiter 并通过无锁头插法加入等待链表。
///
/// P1-2 fix: 插入链表后，re-check counter_。
/// 如果在 await_ready 和此处之间有 release 发生，counter_ 可能已 > 0。
/// 此时 CAS 递减消费该剩余资源，设置 consumed_ 标志，并自唤醒。
/// 这避免了"release 在 awaiter 入链表前执行"导致的丢失唤醒。
///
/// Insert into wait list, then re-check counter to avoid lost wakeup.
void counting_semaphore::acquire_awaiter::await_suspend(
    std::coroutine_handle<> current) noexcept
{
    this->handle_ = current;

    // 无锁头插法插入等待链表
    acquire_awaiter* old_head = sem_.awaiting_.load(std::memory_order_relaxed);
    do {
        this->next_ = old_head;
    } while (!sem_.awaiting_.compare_exchange_weak(
        old_head, this, std::memory_order_release, std::memory_order_relaxed));

    // P1-2 fix: re-check counter after insertion.
    // 如果 release 在 await_ready 和此处之间执行，counter_ 可能已 > 0。
    // CAS 递减消费剩余资源，设置 consumed_ 防止 release 双重唤醒。
    T old = sem_.counter_.load(std::memory_order_acquire);
    while (old > 0) {
        if (sem_.counter_.compare_exchange_weak(
                old, old - 1,
                std::memory_order_acquire,
                std::memory_order_relaxed)) {
            // 消费了剩余资源，标记 consumed 并自唤醒
            this->consumed_.store(true, std::memory_order_release);
            this->co_spawn();
            return;
        }
    }
    // counter_ <= 0：没有剩余资源，正常等待 release 唤醒
}

/// 通过 io_context 恢复协程执行。
void counting_semaphore::acquire_awaiter::co_spawn() const noexcept {
    this->resume_ctx_->spawn_handle(this->handle_);
}

} // namespace coronet
