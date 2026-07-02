#pragma once

#include <atomic>
#include <cstdint>
#include <cassert>

namespace coronet::detail {

/*
 * 无锁 SPSC（单生产者/单消费者）环形缓冲区游标。
 *
 * 为什么需要 SPSC ring：
 * - io_context 的事件循环中，需要将 I/O 完成事件和 co_spawn 的任务
 *   从"产生者"传递到"消费者"，且两者在同一个线程中操作
 * - 标准线程安全的队列（如 mutex+queue）会有锁竞争，虽然 SPSC 的
 *   场景下锁竞争很轻微，但我们的目标是完全无锁
 * - SPSC ring 利用了"只有一个生产者和一个消费者"的约束，只需要两个原子变量
 *   （head 和 tail）即可实现无锁的入队/出队
 * - 比 MPSC/MCMC 队列简单得多，不需要无锁链表或复杂的 CAS 循环
 *
 * 算法原理：
 * - head：消费者读取/写入（pop 时递增），生产者读取（检查是否满）
 * - tail：生产者写入（push 时递增），消费者读取（检查是否空）
 * - 容量为 2 的幂，取模用位运算 (idx & mask) 替代除法
 * - head 和 tail 始终递增（利用 uint32_t 的自然溢出回绕），
 *   通过 head - tail 计算队列长度
 *
 * 模板参数设计：
 * - CurT：游标类型（uint32_t 或 uint64_t），uint32_t 在 x86 上更高效
 * - capacity：固定容量，编译期常量，2 的幂
 * - is_safe：是否使用原子操作。在同一个线程内（如 IO 完成事件
 *   在同一个 io_context 线程中入队和出队）可以关闭原子操作以提升性能。
 *   跨线程场景（如跨 io_context 的 co_spawn）需要启用。
 *
 * 为什么 is_safe 用编译期参数而非运行时：
 * - 原子操作即使不被争用也有额外的指令开销（如内存屏障）
 * - 编译期选择让编译器可以完全优化掉不需要的原子指令
 * - std::atomic 在单线程场景下不会自动降级为普通操作
 *
 * 为什么 head 和 tail 用条件类型：
 * - 使用 std::conditional_t 在 safe/unsafe 之间切换类型
 * - unsafe 模式下直接使用原生整数类型，无原子操作开销
 * - [[no_unique_address]] 使该成员在不使用时占用零额外空间
 *
 * 空/满判断：
 * - 空：head == tail（游标相遇）
 * - 满：tail - head >= capacity（队列满）
 * - 注意：因为 head 和 tail 单调递增，所以必须保证 tail - head
 *   在游标类型回绕前不会溢出。对于 uint32_t 和 capacity=16384，
 *   在 2^32 / 16384 ≈ 262K 次回绕前都不会有问题。
 */
/// Lock-free single-producer single-consumer ring-buffer cursor.
///
/// @tparam CurT     integer cursor type (uint32_t or uint64_t)
/// @tparam capacity ring capacity (must be power of two)
/// @tparam is_safe  if true, uses std::atomic operations; otherwise plain loads/stores
///
/// The ring uses a single atomic head (read by consumer, written by producer)
/// and a single atomic tail (written by consumer, read by producer).
template<typename CurT, CurT capacity, bool is_safe = false>
struct spsc_cursor {
    static_assert((capacity & (capacity - 1)) == 0,
                  "capacity must be a power of two");

    using cur_t = CurT;

    static constexpr cur_t mask = capacity - 1;

    /// Pop an element from the ring head (consumer side).
    /// Returns the element, or -1 if empty.
    /// 从环头部弹出元素（消费者侧）。返回元素索引，空时返回 -1。
    constexpr cur_t pop() noexcept {
        cur_t h = head();
        if (h == tail()) [[unlikely]]
            return cur_t(-1);
        set_head(h + 1);
        return h & mask;  // Return masked slot index (not raw head)
    }

    /// Push an element at the ring tail (producer side).
    /// Returns the slot index, or -1 if full.
    /// 在环尾部压入元素（生产者侧）。返回槽索引，满时返回 -1。
    constexpr cur_t push() noexcept {
        cur_t t = tail();
        if (t - head() >= capacity) [[unlikely]]
            return cur_t(-1);
        set_tail(t + 1);
        return t & mask;
    }

    /// Current size (number of elements in the ring)
    /// 当前环大小（元素数量）
    constexpr cur_t size() const noexcept {
        return tail() - head();
    }

    /// True if empty
    /// 是否为空
    constexpr bool empty() const noexcept {
        return head() == tail();
    }

    // --- atomic / non-atomic accessors ---

    constexpr cur_t head() const noexcept {
        if constexpr (is_safe) return head_.load(std::memory_order_acquire);
        else                   return head_;
    }

    constexpr void set_head(cur_t h) noexcept {
        if constexpr (is_safe) head_.store(h, std::memory_order_release);
        else                   head_ = h;
    }

    constexpr cur_t tail() const noexcept {
        if constexpr (is_safe) return tail_.load(std::memory_order_acquire);
        else                   return tail_;
    }

    constexpr void set_tail(cur_t t) noexcept {
        if constexpr (is_safe) tail_.store(t, std::memory_order_release);
        else                   tail_ = t;
    }

private:
    // On x86 plain loads/stores are already acquire/release (for aligned integers),
    // but for cross-platform correctness we use atomic unconditionally when is_safe.
    // x86 上普通的对齐整数加载/存储已经是 acquire/release 语义，
    // 但为了跨平台正确性，is_safe 时无条件使用原子操作。
    [[no_unique_address]]
    std::conditional_t<is_safe, std::atomic<cur_t>, cur_t> head_{0};

    [[no_unique_address]]
    std::conditional_t<is_safe, std::atomic<cur_t>, cur_t> tail_{0};
};

} // namespace coronet::detail
