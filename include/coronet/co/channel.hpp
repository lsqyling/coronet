#pragma once

#include "coronet/co/condition_variable.hpp"
#include "coronet/co/mutex.hpp"
#include "coronet/detail/uninitialized_buffer.hpp"
#include "coronet/task.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

namespace coronet {

/// CSP-style channel for coroutine communication.
/// @tparam T        element type (must be move-constructible)
/// @tparam capacity 0 = rendezvous, 1 = single-slot buffer, N = N-slot buffer
template<std::move_constructible T, size_t capacity = 0>
class channel {
    static_assert(capacity != 0 && capacity != 1,
                  "use channel<T,0> for rendezvous, channel<T,1> for single-slot");

public:
    channel() = default;

    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool full() const noexcept { return size_ == capacity; }

    /// Remove and discard the front item
    task<> drop() {
        co_await mtx_.lock();
        if (empty()) {
            co_await not_empty_cv_.wait(mtx_, [this] { return !this->empty(); });
        }
        std::destroy_at(first_);
        pop_one();
        mtx_.unlock();
        not_full_cv_.notify_one();
    }

    /// Take an item from the channel (awaits if empty)
    task<T> acquire() {
        co_await mtx_.lock();
        if (empty()) {
            co_await not_empty_cv_.wait(mtx_, [this] { return !this->empty(); });
        }
        T item{std::move(*first_)};
        std::destroy_at(first_);
        pop_one();
        mtx_.unlock();
        not_full_cv_.notify_one();
        co_return std::move(item);
    }

    /// Put an item into the channel (awaits if full)
    template<typename... Args>
    task<> release(Args&&... args) {
        co_await mtx_.lock();
        if (full()) {
            co_await not_full_cv_.wait(mtx_, [this] { return !this->full(); });
        }
        std::construct_at(last_, std::forward<Args>(args)...);
        push_one();
        mtx_.unlock();
        not_empty_cv_.notify_one();
    }

private:
    std::array<detail::uninitialized_buffer<T>, capacity> buf_;
    T* first_{reinterpret_cast<T*>(buf_.data())};
    T* last_{reinterpret_cast<T*>(buf_.data())};
    size_t size_{0};

    mutex mtx_;
    condition_variable not_full_cv_;
    condition_variable not_empty_cv_;

    const T* buffer_start() const noexcept { return reinterpret_cast<const T*>(buf_.data()); }
    const T* buffer_end()   const noexcept { return reinterpret_cast<const T*>(&(*buf_.end())); }
    T* buffer_start() noexcept { return reinterpret_cast<T*>(buf_.data()); }
    T* buffer_end()   noexcept { return reinterpret_cast<T*>(&(*buf_.end())); }

    void pop_one() noexcept {
        ++first_; --size_;
        if (first_ == buffer_end()) first_ = buffer_start();
    }
    void push_one() noexcept {
        ++last_; ++size_;
        if (last_ == buffer_end()) last_ = buffer_start();
    }
};

// ---- Single-slot buffer specialization (capacity == 1) ----

template<std::move_constructible T>
class channel<T, 1> {
public:
    channel() = default;

    [[nodiscard]] size_t size() const noexcept { return buf_.has_value() ? 1 : 0; }
    [[nodiscard]] bool empty() const noexcept { return !buf_.has_value(); }
    [[nodiscard]] bool full() const noexcept { return buf_.has_value(); }

    task<> drop() {
        co_await mtx_.lock();
        if (empty()) {
            co_await not_empty_cv_.wait(mtx_, [this] { return this->full(); });
        }
        buf_.reset();
        mtx_.unlock();
        not_full_cv_.notify_one();
    }

    task<T> acquire() {
        co_await mtx_.lock();
        if (empty()) {
            co_await not_empty_cv_.wait(mtx_, [this] { return this->full(); });
        }
        T item{std::move(*buf_)};
        buf_.reset();
        mtx_.unlock();
        not_full_cv_.notify_one();
        co_return std::move(item);
    }

    template<typename... Args>
    task<> release(Args&&... args) {
        co_await mtx_.lock();
        if (full()) {
            co_await not_full_cv_.wait(mtx_, [this] { return !this->full(); });
        }
        buf_.emplace(std::forward<Args>(args)...);
        mtx_.unlock();
        not_empty_cv_.notify_one();
    }

private:
    std::optional<T> buf_;
    mutex mtx_;
    condition_variable not_full_cv_;
    condition_variable not_empty_cv_;
};

// ---- Rendezvous specialization (capacity == 0) ----

template<std::move_constructible T>
class channel<T, 0> {
public:
    channel() = default;

    [[nodiscard]] constexpr size_t size() const noexcept { return 0; }
    [[nodiscard]] constexpr bool empty() const noexcept { return true; }
    [[nodiscard]] constexpr bool full() const noexcept { return false; }

    task<T> acquire() {
        // Rendezvous: wait for a release, match directly
        co_await mtx_.lock();
        if (!released_) {
            co_await release_cv_.wait(mtx_, [this] { return released_; });
        }
        T item{std::move(stored_)};
        released_ = false;
        mtx_.unlock();
        acquired_cv_.notify_one();
        co_return std::move(item);
    }

    template<typename... Args>
    task<> release(Args&&... args) {
        co_await mtx_.lock();
        stored_ = T(std::forward<Args>(args)...);
        released_ = true;
        mtx_.unlock();
        release_cv_.notify_one();

        // Wait for acquire to take the value
        if (released_) {
            co_await acquired_cv_.wait(mtx_, [this] { return !released_; });
        }
    }

private:
    T stored_{};
    bool released_ = false;
    mutex mtx_;
    condition_variable release_cv_;
    condition_variable acquired_cv_;
};

} // namespace coronet
