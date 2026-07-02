#include "coronet/co/mutex.hpp"
#include "coronet/io_context.hpp"

#include <cassert>

namespace coronet {

mutex::~mutex() noexcept {
    // 析构时验证：锁必须已释放，且无残留等待者。
    // 这是重要的契约检查 —— 持有协程锁时不能销毁锁对象。
    [[maybe_unused]] auto state = awaiting_.load(std::memory_order_relaxed);
    assert(state == not_locked || state == locked_no_awaiting);
    assert(to_resume_ == nullptr);
}

bool mutex::try_lock() noexcept {
    // 尝试将 not_locked → locked_no_awaiting，失败则锁已被持有。
    // 这是无等待路径，用于不需要挂起的场景。
    auto desire = not_locked;
    return awaiting_.compare_exchange_strong(
        desire, locked_no_awaiting, std::memory_order_acquire,
        std::memory_order_relaxed);
}

/// 释放互斥锁并唤醒等待者。
///
/// ## 算法详解
///
/// unlock 分为三个阶段：
///
/// 1. 如果 to_resume_ 非空（说明上一次 unlock 反转的链表尚未消费完），
///    直接取出头节点即可恢复，无需 CAS 操作。
///
/// 2. 如果 to_resume_ 为空且 awaiting_ 还是 locked_no_awaiting，
///    说明没有等待者，直接 CAS 设为 not_locked 即可返回。
///
/// 3. 如果 to_resume_ 为空但 awaiting_ 指向一个等待者链表，
///    说明有协程在等待锁。需要：
///    a. 将 awaiting_ 用 exchange 置为 locked_no_awaiting（表示锁被当前线程持有）
///    b. 反转从 awaiting_ 取出的链表（头插法导致顺序反转，反转后恢复 FIFO 顺序）
///    c. 取出反转后链表的头节点，通过 co_spawn 投递给调度器执行
///    d. 剩余节点存入 to_resume_，等待后续 unlock 消费
///
/// 为什么用 co_spawn 而不是直接 resume？
/// 直接 resume 会在当前线程（unlock 发起者）上同步执行协程恢复，可能导致：
///   - 调用栈过深（如果协程链很长）
///   - 违反调度公平性（应该由等待协程所在的 io_context 调度）
/// co_spawn 将协程句柄投递到其所在 io_context 的任务队列，由事件循环择机执行。
void mutex::unlock() noexcept {
    assert(awaiting_.load(std::memory_order_relaxed) != not_locked);
    lock_awaiter* resume_head = to_resume_;
    if (resume_head == nullptr) {
        // 没有缓存的等待者：检查是否需要处理新的等待者
        auto desire = locked_no_awaiting;
        if (awaiting_.compare_exchange_strong(
                desire, not_locked, std::memory_order_release,
                std::memory_order_relaxed)) {
            // 无等待者，直接标记为未锁定
            return;
        }
        // 有等待者：取出整个链表，反转以恢复 FIFO 顺序
        auto top = awaiting_.exchange(locked_no_awaiting, std::memory_order_acquire);
        assert(top != not_locked && top != locked_no_awaiting);
        auto* node = std::assume_aligned<alignof(lock_awaiter)>(
            reinterpret_cast<lock_awaiter*>(top));
        // 反转链表：由于插入是头插法，取出时的顺序是 LIFO，
        // 反转后变为 FIFO，保证先等待的协程优先获取锁。
        do {
            lock_awaiter* tmp = node->next_;
            node->next_ = resume_head;
            resume_head = node;
            node = tmp;
        } while (node != nullptr);
    }
    assert(resume_head != nullptr);
    // 取出反转后链表的第一个节点，剩余缓存到 to_resume_ 供下次 unlock 使用
    to_resume_ = resume_head->next_;
    resume_head->co_spawn();
}

/// 将当前协程注册到互斥锁的等待队列。
///
/// ## CAS 循环详解
///
/// 这是一个经典的 lock-free 链表插入 + 状态检测的 CAS 循环。
///
/// 读取 awaiting_ 的当前值，分两种情况处理：
///
///   - not_locked（锁空闲）：尝试 CAS 锁定，成功则返回 false（不挂起，直接获取锁）
///   - 其他值（锁被持有或已有等待者）：将当前 awaiter 的 next 指向旧链表头，
///     然后 CAS 尝试将 awaiting_ 更新为指向当前 awaiter。
///     如果 CAS 成功，返回 true（挂起等待）；如果失败，重试整个循环。
///
/// 这种设计的关键优势在于：锁状态和等待队列头共用一个原子变量，
/// 避免了 ABA 问题和额外的状态同步开销。
bool mutex::lock_awaiter::register_awaiting() noexcept {
    uintptr_t old_state = mtx_.awaiting_.load(std::memory_order_acquire);
    while (true) {
        if (old_state == mutex::not_locked) {
            // 锁空闲：尝试获取锁
            if (mtx_.awaiting_.compare_exchange_weak(
                    old_state, mutex::locked_no_awaiting,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                return false;  // 获取成功，不挂起
            }
        } else {
            // 锁被持有：加入等待队列（头插法）
            this->next_ = std::assume_aligned<alignof(lock_awaiter)>(
                reinterpret_cast<lock_awaiter*>(old_state));
            if (mtx_.awaiting_.compare_exchange_weak(
                    old_state, reinterpret_cast<uintptr_t>(this),
                    std::memory_order_release, std::memory_order_relaxed)) {
                return true;  // 入队成功，挂起
            }
        }
    }
}

/// 通过 io_context 的 spawn 机制恢复协程，而非直接 resume。
///
/// 这保证了：
/// 1. 协程在其原始 io_context 线程上恢复，保持局部性
/// 2. 恢复操作被延迟到事件循环的下一次迭代，避免递归调用栈过深
/// 3. 与 io_context 的任务调度机制集成，支持跨线程唤醒
void mutex::lock_awaiter::co_spawn() const noexcept {
    this->resume_ctx_->spawn_handle(this->awaken_coro_);
}

} // namespace coronet
