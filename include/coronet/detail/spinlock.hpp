#pragma once

#include <atomic>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace coronet::detail {

/*
 * 简单的自旋锁 —— 使用 std::atomic_flag 实现。
 *
 * 为什么使用自旋锁而非标准 mutex：
 * - 在协程库中，许多锁的持有时间非常短（仅保护几个原子变量赋值），
 *   此时线程睡眠/唤醒的开销（约微秒级）远大于自旋几个循环的开销（纳秒级）。
 * - 自旋锁在"预期很快能获取到锁"的场景下性能远优于互斥锁。
 * - 协程库的某些路径（如跨线程 co_spawn）不能阻塞线程，
 *   因为阻塞线程可能会阻塞其他协程的调度。
 *
 * 为什么使用 std::atomic_flag 而非 std::atomic<bool>：
 * - atomic_flag 保证是锁自由的（lock-free），而 atomic<bool> 不保证。
 * - atomic_flag::test_and_set 是原子的"测试并设置"操作，硬件直接支持。
 * - atomic_flag 的设计就是用于实现自旋锁这样的低级原语。
 *
 * 内存序说明：
 * - lock() 用 memory_order_acquire：确保锁之前的读操作不会重排到锁之后
 * - unlock() 用 memory_order_release：确保临界区的写操作在解锁前对其他线程可见
 * - try_lock() 用 memory_order_acquire：与 lock() 一致
 * - 这是自旋锁的标准内存序模式，提供互斥和 happens-before 保证
 *
 * CPU 暂停指令：
 * - x86 上的 _mm_pause() 提示处理器当前处于自旋等待循环，
 *   让处理器优化流水线（减少功耗、提高超线程同伴的性能）。
 * - ARM 上的 yield 提示相当于 WFE（Wait For Event）。
 * - 这些指令不是必需的，但能显著改善自旋等待的功耗和性能。
 *
 * 使用约束：
 * - 绝对不要在持有自旋锁时执行阻塞操作（I/O、锁获取、内存分配）
 * - 临界区应尽量短，否则应使用标准互斥锁
 * - 单线程场景下可安全使用（test_and_set 返回 false，无需自旋）
 */
/// A simple spinlock using std::atomic_flag.
/// Intended for short critical sections only; never block inside a spinlock.
/// 适用于短临界区，切勿在持有自旋锁时执行阻塞操作。
class spinlock {
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;

public:
    void lock() noexcept {
        while (flag_.test_and_set(std::memory_order_acquire)) {
            // busy-wait; on x86 this is a pause instruction
            // 忙等待循环，x86 上使用 pause 指令降低功耗
            #if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
            #elif defined(__aarch64__) || defined(_M_ARM64)
                __asm__ __volatile__("yield" ::: "memory");
            #endif
        }
    }

    bool try_lock() noexcept {
        return !flag_.test_and_set(std::memory_order_acquire);
    }

    void unlock() noexcept {
        flag_.clear(std::memory_order_release);
    }
};

} // namespace coronet::detail
