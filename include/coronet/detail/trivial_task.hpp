#pragma once

#include <coroutine>

namespace coronet::detail {

/// Minimal coroutine type for condition_variable's predicate wait.
class trivial_task {
public:
    struct promise_type;

    explicit trivial_task(std::coroutine_handle<promise_type> self_handle) noexcept
        : handle(self_handle) {}

    struct final_awaiter {
        static constexpr bool await_ready() noexcept { return false; }

        static std::coroutine_handle<>
        await_suspend(std::coroutine_handle<promise_type> current) noexcept {
            auto continuation = current.promise().parent_coro;
            current.destroy();
            return continuation;
        }

        static constexpr void await_resume() noexcept {}
    };

    struct promise_type {
        static constexpr std::suspend_always initial_suspend() noexcept { return {}; }
        static constexpr final_awaiter final_suspend() noexcept { return {}; }

        std::coroutine_handle<> parent_coro;

        trivial_task get_return_object() noexcept {
            return trivial_task{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        constexpr void unhandled_exception() noexcept {}
        constexpr void return_void() noexcept {}
    };

    static constexpr bool await_ready() noexcept { return false; }

    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> awaiting_coro) const noexcept {
        handle.promise().parent_coro = awaiting_coro;
        return handle;
    }

    static constexpr void await_resume() noexcept {}

    std::coroutine_handle<promise_type> handle;
};

} // namespace coronet::detail
