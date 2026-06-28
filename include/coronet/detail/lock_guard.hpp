#pragma once

#include <type_traits>

namespace coronet::detail {

/// RAII lock guard for any type with unlock() method.
/// NOTE: the mutex must already be locked before constructing this guard.
template<typename Lockable>
class lock_guard {
    static_assert(!std::is_reference_v<Lockable>,
                  "lock_guard<Lockable&> is not allowed; store a pointer or use std::ref");

    Lockable* lock_;

public:
    /// Construct a guard for an already-locked mutex
    constexpr explicit lock_guard(Lockable& lk) noexcept
        : lock_(std::addressof(lk)) {}

    constexpr ~lock_guard() noexcept {
        if (lock_) lock_->unlock();
    }

    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;
    lock_guard(lock_guard&&) = delete;
    lock_guard& operator=(lock_guard&&) = delete;
};

} // namespace coronet::detail
