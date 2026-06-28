#pragma once

#include "coronet/co/mutex.hpp"
#include "coronet/detail/spinlock.hpp"
#include "coronet/detail/thread_meta.hpp"
#include "coronet/detail/trivial_task.hpp"

#include <atomic>
#include <coroutine>

namespace coronet {

class condition_variable;

namespace detail {

class [[nodiscard("Did you forget to co_await?")]] cv_wait_awaiter final {
public:
    using mutex = coronet::mutex;

    explicit cv_wait_awaiter(condition_variable& cv, mutex& mtx) noexcept
        : lock_awaken_handle_(mtx.lock())
        , cv_(cv) {}

    static constexpr bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> current) noexcept;
    constexpr void await_resume() const noexcept {}

private:
    mutex::lock_awaiter lock_awaken_handle_;
    condition_variable& cv_;
    cv_wait_awaiter* next_ = nullptr;

    friend class ::coronet::condition_variable;
};

} // namespace detail

class condition_variable final {
public:
    using cv_wait_awaiter = detail::cv_wait_awaiter;

    condition_variable() noexcept = default;
    ~condition_variable() noexcept = default;

    cv_wait_awaiter wait(mutex& mtx) noexcept {
        return cv_wait_awaiter{*this, mtx};
    }

    template<std::predicate Pred>
    detail::trivial_task wait(mutex& mtx, Pred stop_waiting) {
        while (!stop_waiting()) {
            co_await this->wait(mtx);
        }
    }

    void notify_one() noexcept;
    void notify_all() noexcept;

private:
    friend class detail::cv_wait_awaiter;

    std::atomic<cv_wait_awaiter*> awaiting_{nullptr};
    cv_wait_awaiter* to_resume_head_ = nullptr;
    cv_wait_awaiter* to_resume_tail_ = nullptr;
    detail::spinlock notifier_mtx_;
};

} // namespace coronet
