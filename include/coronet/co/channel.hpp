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

/// CSP（Communicating Sequential Processes）风格的信道 —— 用于协程间通信。
///
/// CSP-style channel for coroutine communication.
///
/// ## 设计来源：CSP 模型与 Go channel
///
/// 本实现参考了 Go 语言的 channel 设计，提供协程间的安全通信机制。
/// 与 Go 不同的是，所有操作都是异步且类型安全的（通过 C++ 模板实现）。
///
/// ## 三种特化
///
/// 根据模板参数 capacity，channel 有三种实现：
///
///   0: rendezvous（会合模式）—— 发送者和接收者必须同时就绪，
///      数据直接从发送者传到接收者，零缓冲、零拷贝（移动语义）。
///      最适合协程间同步和信号传递。
///
///   1: single-slot（单槽缓冲）—— 最多缓存一个元素。
///      使用 std::optional<T> 作为存储，实现简洁高效。
///      适合生产者-消费者速率不完全匹配的场景。
///
///   N: N-slot（N 槽缓冲）—— 使用环形缓冲区存储 N 个元素。
///      首个和最后一个元素的指针在数组中循环移动。
///      适合有较大缓冲需求的流式处理场景。
///
/// ## 线程安全
///
/// 所有操作都通过内部的 mutex 保证线程安全。condition_variable 用于
/// 在缓冲区满（或空）时挂起调用协程，有数据可用（或空间可用）时唤醒。
///
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

    /// 移除并销毁队首元素（不返回）。
    ///
    /// Remove and discard the front item.
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

    /// 从信道取一个元素（缓冲区为空时挂起）。
    ///
    /// Take an item from the channel (awaits if empty).
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

    /// 向信道放入一个元素（缓冲区满时挂起）。
    ///
    /// Put an item into the channel (awaits if full).
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
    // 环形缓冲区：在固定大小的数组中循环使用槽位
    std::array<detail::uninitialized_buffer<T>, capacity> buf_;
    T* first_{reinterpret_cast<T*>(buf_.data())};  // 队首元素位置
    T* last_{reinterpret_cast<T*>(buf_.data())};    // 队尾（下一个写入位置）
    size_t size_{0};

    mutex mtx_;
    condition_variable not_full_cv_;   // 等待"不满"条件
    condition_variable not_empty_cv_;  // 等待"不空"条件

    const T* buffer_start() const noexcept { return reinterpret_cast<const T*>(buf_.data()); }
    const T* buffer_end()   const noexcept { return reinterpret_cast<const T*>(buf_.data() + capacity); }
    T* buffer_start() noexcept { return reinterpret_cast<T*>(buf_.data()); }
    T* buffer_end()   noexcept { return reinterpret_cast<T*>(buf_.data() + capacity); }

    // 出队：first_ 前进一位，到达末尾则绕回开头
    void pop_one() noexcept {
        ++first_; --size_;
        if (first_ == buffer_end()) first_ = buffer_start();
    }
    // 入队：last_ 前进一位，到达末尾则绕回开头
    void push_one() noexcept {
        ++last_; ++size_;
        if (last_ == buffer_end()) last_ = buffer_start();
    }
};

// ---- Single-slot buffer specialization (capacity == 1) ----
//
// 单槽缓冲特化：使用 std::optional<T> 替代环形缓冲区，实现更简洁。
// 当 buf_ 有值时表示满，无值时表示空。利用 optional 的 has_value()
// 直接判断状态，无需维护独立的 size_ 和指针。

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
//
// 会合模式：零缓冲，发送者和接收者直接握手。
// 发送者将数据存入 stored_，设置 released_ = true，通知接收者。
// 发送者随后等待接收者取走数据（等待 acquired_cv_），确保数据被消费。
// 接收者取走数据后设置 released_ = false，通知发送者可以继续。
//
// 这种双向握手保证了：数据在发送者恢复执行之前已被安全地移动给接收者。
// 适合协程间精确同步的场景，如任务分配、事件通知等。

template<std::move_constructible T>
class channel<T, 0> {
public:
    channel() = default;

    // P2-1 fix: 析构时销毁缓冲区中可能存在的 T（如果 released_ 为 true）。
    // 旧实现用 T stored_{}（自动析构），改用 uninitialized_buffer 后需手动管理。
    ~channel() {
        if (released_.load(std::memory_order_relaxed)) {
            stored_buf_.destroy();
        }
    }

    [[nodiscard]] constexpr size_t size() const noexcept { return 0; }
    [[nodiscard]] constexpr bool empty() const noexcept { return true; }
    [[nodiscard]] constexpr bool full() const noexcept { return false; }

    /// 从信道接收数据 —— 在 rendezvous 模式下，需等待发送者就绪。
    ///
    /// Receive data — in rendezvous mode, wait for a sender.
    task<T> acquire() {
        co_await mtx_.lock();
        if (!released_.load(std::memory_order_relaxed)) {
            co_await release_cv_.wait(mtx_, [this] {
                return released_.load(std::memory_order_relaxed);
            });
        }
        // P2-1 fix: 从 uninitialized_buffer 移出数据，然后销毁
        T item{std::move(stored_buf_.ref())};
        stored_buf_.destroy();
        released_.store(false, std::memory_order_release);
        mtx_.unlock();
        acquired_cv_.notify_one();
        co_return std::move(item);
    }

    /// 向信道发送数据 —— 在 rendezvous 模式下，需等待接收者取走。
    ///
    /// Send data — in rendezvous mode, wait for receiver to take it.
    template<typename... Args>
    task<> release(Args&&... args) {
        co_await mtx_.lock();
        // P2-1 fix: 用 construct 替代赋值（uninitialized_buffer 不需要 T 可赋值）
        stored_buf_.construct(std::forward<Args>(args)...);
        released_.store(true, std::memory_order_release);
        mtx_.unlock();
        release_cv_.notify_one();

        if (released_.load(std::memory_order_acquire)) {
            co_await acquired_cv_.wait(mtx_, [this] {
                return !released_.load(std::memory_order_relaxed);
            });
        }
    }

private:
    // P2-1 fix: 使用 uninitialized_buffer 替代 T stored_{}
    // 消除对 T 的 default-constructible 和 assignable 要求，
    // 与 N-slot 和 single-slot 特化保持一致（仅要求 move_constructible）。
    detail::uninitialized_buffer<T> stored_buf_;
    std::atomic<bool> released_{false};  // 发送者已完成放置（atomic：release() 在 unlock 后读取）
    mutex mtx_;
    condition_variable release_cv_;  // 等待发送者信号
    condition_variable acquired_cv_; // 等待接收者确认
};

} // namespace coronet
