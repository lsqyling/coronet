#pragma once

#include <atomic>
#include <cstdint>
#include <cassert>

namespace coronet::detail {

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
    constexpr cur_t pop() noexcept {
        cur_t h = head();
        if (h == tail()) [[unlikely]]
            return cur_t(-1);
        set_head(h + 1);
        return h & mask;  // Return masked slot index (not raw head)
    }

    /// Push an element at the ring tail (producer side).
    /// Returns the slot index, or -1 if full.
    constexpr cur_t push() noexcept {
        cur_t t = tail();
        if (t - head() >= capacity) [[unlikely]]
            return cur_t(-1);
        set_tail(t + 1);
        return t & mask;
    }

    /// Current size (number of elements in the ring)
    constexpr cur_t size() const noexcept {
        return tail() - head();
    }

    /// True if empty
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
    [[no_unique_address]]
    std::conditional_t<is_safe, std::atomic<cur_t>, cur_t> head_{0};

    [[no_unique_address]]
    std::conditional_t<is_safe, std::atomic<cur_t>, cur_t> tail_{0};
};

} // namespace coronet::detail
