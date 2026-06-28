#pragma once

#include "coronet/detail/thread_meta.hpp"
#include "coronet/io_context.hpp"

#include <atomic>
#include <cassert>
#include <coroutine>
#include <exception>
#include <memory>
#include <type_traits>

namespace coronet {

template<typename T>
class shared_task;

namespace detail {

struct shared_task_waiter {
    std::coroutine_handle<> continuation;
    io_context* resume_ctx;
    shared_task_waiter* next;
};

class shared_task_promise_base {
    friend struct final_awaiter;

    struct final_awaiter {
        static constexpr bool await_ready() noexcept { return false; }

        template<typename Promise>
        void await_suspend(std::coroutine_handle<Promise> h) noexcept {
            auto& promise = h.promise();
            void* const value_ready = &promise;
            void* waiters = promise.waiters_.exchange(
                value_ready, std::memory_order_acq_rel);
            if (waiters != nullptr) {
                auto* waiter = static_cast<shared_task_waiter*>(waiters);
                do {
                    waiter->resume_ctx->spawn_handle(waiter->continuation);
                    waiter = waiter->next;
                } while (waiter != nullptr);
            }
        }
        void await_resume() noexcept {}
    };

public:
    shared_task_promise_base() noexcept
        : ref_count_(1)
        , waiters_(&waiters_) {}

    constexpr std::suspend_always initial_suspend() noexcept { return {}; }
    final_awaiter final_suspend() noexcept { return {}; }

    void unhandled_exception() noexcept {
        exception_ = std::current_exception();
    }

    bool is_ready() const noexcept {
        return waiters_.load(std::memory_order_acquire) == this;
    }

    void add_ref() noexcept {
        ref_count_.fetch_add(1, std::memory_order_relaxed);
    }

    bool try_detach() noexcept {
        return ref_count_.fetch_sub(1, std::memory_order_acq_rel) != 1;
    }

    bool try_await(shared_task_waiter* waiter, std::coroutine_handle<> coroutine) {
        void* const value_ready = this;
        void* const not_started = &waiters_;
        void* const started_empty = static_cast<shared_task_waiter*>(nullptr);

        void* old_waiters = waiters_.load(std::memory_order_acquire);
        if (old_waiters == not_started &&
            waiters_.compare_exchange_strong(
                old_waiters, started_empty, std::memory_order_relaxed)) {
            coroutine.resume();
            old_waiters = waiters_.load(std::memory_order_acquire);
        }

        do {
            if (old_waiters == value_ready) return false;
            waiter->next = static_cast<shared_task_waiter*>(old_waiters);
        } while (!waiters_.compare_exchange_weak(
            old_waiters, static_cast<void*>(waiter),
            std::memory_order_release, std::memory_order_acquire));

        return true;
    }

protected:
    bool has_exception() const noexcept { return exception_ != nullptr; }
    void rethrow_exception() {
        if (exception_) std::rethrow_exception(exception_);
    }

private:
    std::atomic<uint32_t> ref_count_;
    std::atomic<void*> waiters_;
    std::exception_ptr exception_;
};

template<typename T>
class shared_task_promise final : public shared_task_promise_base {
public:
    shared_task_promise() noexcept = default;

    ~shared_task_promise() {
        if (is_ready() && !has_exception()) {
            reinterpret_cast<T*>(&storage_)->~T();
        }
    }

    shared_task<T> get_return_object() noexcept;

    template<typename U>
        requires std::is_convertible_v<U&&, T>
    void return_value(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
        new (&storage_) T(std::forward<U>(value));
    }

    T& result() {
        rethrow_exception();
        return *reinterpret_cast<T*>(&storage_);
    }

private:
    alignas(T) char storage_[sizeof(T)];
};

template<>
class shared_task_promise<void> final : public shared_task_promise_base {
public:
    shared_task<void> get_return_object() noexcept;
    void return_void() noexcept {}
    void result() { rethrow_exception(); }
};

template<typename T>
class shared_task_promise<T&> final : public shared_task_promise_base {
public:
    shared_task<T&> get_return_object() noexcept;
    void return_value(T& value) noexcept { ptr_ = &value; }
    T& result() { rethrow_exception(); return *ptr_; }
private:
    T* ptr_ = nullptr;
};

} // namespace detail

template<typename T = void>
class [[nodiscard]] shared_task {
public:
    using promise_type = detail::shared_task_promise<T>;
    using value_type = T;
    struct is_task_like {};
    struct is_shared_task {};

private:
    struct awaitable_base {
        std::coroutine_handle<promise_type> coroutine_;
        detail::shared_task_waiter waiter_;

        awaitable_base(std::coroutine_handle<promise_type> c) noexcept
            : coroutine_(c) {}

        bool await_ready() const noexcept {
            return !coroutine_ || coroutine_.promise().is_ready();
        }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            waiter_.continuation = awaiter;
            waiter_.resume_ctx = detail::this_thread.ctx;
            return coroutine_.promise().try_await(&waiter_, coroutine_);
        }
    };

public:
    shared_task() noexcept : coroutine_(nullptr) {}
    explicit shared_task(std::coroutine_handle<promise_type> c) noexcept
        : coroutine_(c) {}

    shared_task(shared_task&& other) noexcept
        : coroutine_(other.coroutine_) { other.coroutine_ = nullptr; }

    shared_task(const shared_task& other) noexcept
        : coroutine_(other.coroutine_) {
        if (coroutine_) coroutine_.promise().add_ref();
    }

    ~shared_task() { destroy(); }

    shared_task& operator=(shared_task&& other) noexcept {
        if (this != &other) {
            destroy();
            coroutine_ = other.coroutine_;
            other.coroutine_ = nullptr;
        }
        return *this;
    }

    shared_task& operator=(const shared_task& other) noexcept {
        if (coroutine_ != other.coroutine_) {
            destroy();
            coroutine_ = other.coroutine_;
            if (coroutine_) coroutine_.promise().add_ref();
        }
        return *this;
    }

    void swap(shared_task& other) noexcept { std::swap(coroutine_, other.coroutine_); }

    /// Get the underlying coroutine handle (nullptr if empty/completed).
    std::coroutine_handle<promise_type> get_handle() const noexcept {
        return coroutine_;
    }

    bool is_ready() const noexcept {
        return !coroutine_ || coroutine_.promise().is_ready();
    }

    auto operator co_await() const noexcept {
        struct awaitable : awaitable_base {
            using awaitable_base::awaitable_base;
            decltype(auto) await_resume() {
                assert(this->coroutine_ && "broken_promise");
                return this->coroutine_.promise().result();
            }
        };
        return awaitable{coroutine_};
    }

    auto when_ready() const noexcept {
        struct awaitable : awaitable_base {
            using awaitable_base::awaitable_base;
            void await_resume() noexcept {}
        };
        return awaitable{coroutine_};
    }

private:
    void destroy() noexcept {
        if (coroutine_ && !coroutine_.promise().try_detach()) {
            coroutine_.destroy();
        }
    }

    std::coroutine_handle<promise_type> coroutine_;
};

// ---- get_return_object implementations ----

namespace detail {
template<typename T>
inline shared_task<T> shared_task_promise<T>::get_return_object() noexcept {
    return shared_task<T>{
        std::coroutine_handle<shared_task_promise>::from_promise(*this)};
}

inline shared_task<void> shared_task_promise<void>::get_return_object() noexcept {
    return shared_task<void>{
        std::coroutine_handle<shared_task_promise>::from_promise(*this)};
}

template<typename T>
inline shared_task<T&> shared_task_promise<T&>::get_return_object() noexcept {
    return shared_task<T&>{
        std::coroutine_handle<shared_task_promise>::from_promise(*this)};
}
} // namespace detail

} // namespace coronet
