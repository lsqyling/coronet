#pragma once

#include "coronet/detail/io_context_meta.hpp"
#include "coronet/detail/lock_guard.hpp"
#include "coronet/detail/thread_meta.hpp"

#include <atomic>
#include <cassert>
#include <coroutine>

namespace coronet::detail {
    class cv_wait_awaiter;
}

namespace coronet {

class io_context;

class mutex final {
public:
    class [[nodiscard("Did you forget to co_await?")]] lock_awaiter {
    public:
        explicit lock_awaiter(mutex& mtx) noexcept
            : mtx_(mtx)
            , resume_ctx_(detail::this_thread.ctx)
        {
            assert(resume_ctx_ != nullptr && "locking mutex without an io_context");
        }

        static constexpr bool await_ready() noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> current) noexcept {
            awaken_coro_ = current;
            return register_awaiting();
        }

        void await_resume() const noexcept {}

    protected:
        void register_coroutine(std::coroutine_handle<> handle) noexcept {
            awaken_coro_ = handle;
        }

        std::coroutine_handle<> get_coroutine() noexcept { return awaken_coro_; }

        bool register_awaiting() noexcept;
        void unlock_ahead() noexcept { mtx_.unlock(); }
        void co_spawn() const noexcept;

        mutex& mtx_;
        lock_awaiter* next_ = nullptr;
        std::coroutine_handle<> awaken_coro_;
        io_context* resume_ctx_;

        friend class mutex;
        friend class condition_variable;
        friend class detail::cv_wait_awaiter;
    };

    class [[nodiscard("Did you forget to co_await?")]] lock_guard_awaiter final
        : public lock_awaiter {
    public:
        using lock_awaiter::lock_awaiter;
        using lock_guard = detail::lock_guard<mutex>;

        [[nodiscard]] lock_guard await_resume() const noexcept {
            return lock_guard{mtx_};
        }
    };

    mutex() noexcept : awaiting_(not_locked) {}
    ~mutex() noexcept;

    bool try_lock() noexcept;
    lock_awaiter lock() noexcept { return lock_awaiter{*this}; }
    lock_guard_awaiter lock_guard() noexcept { return lock_guard_awaiter{*this}; }
    void unlock() noexcept;

private:
    static constexpr uintptr_t locked_no_awaiting = 0;
    static constexpr uintptr_t not_locked = 1;

    std::atomic<uintptr_t> awaiting_;
    lock_awaiter* to_resume_ = nullptr;
};

} // namespace coronet
