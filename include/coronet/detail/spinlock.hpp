#pragma once

#include <atomic>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace coronet::detail {

/// A simple spinlock using std::atomic_flag.
/// Intended for short critical sections only; never block inside a spinlock.
class spinlock {
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;

public:
    void lock() noexcept {
        while (flag_.test_and_set(std::memory_order_acquire)) {
            // busy-wait; on x86 this is a pause instruction
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
