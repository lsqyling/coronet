#pragma once

#include "coronet/config/io_context.hpp"
#include "coronet/detail/spinlock.hpp"
#include "coronet/detail/thread_meta.hpp"

#include <atomic>
#include <coroutine>
#include <type_traits>

namespace coronet {

class io_context;

class counting_semaphore final {
private:
    using T = config::semaphore_counting_t;

    class [[nodiscard("Did you forget to co_await?")]] acquire_awaiter final {
    public:
        explicit acquire_awaiter(counting_semaphore& sem) noexcept
            : sem_(sem)
            , resume_ctx_(detail::this_thread.ctx) {}

        bool await_ready() noexcept {
            T old_counter = sem_.counter_.fetch_sub(1, std::memory_order_acquire);
            return old_counter > 0;
        }

        void await_suspend(std::coroutine_handle<> current) noexcept;
        void await_resume() const noexcept {}

    private:
        void co_spawn() const noexcept;

        counting_semaphore& sem_;
        acquire_awaiter* next_ = nullptr;
        std::coroutine_handle<> handle_;
        io_context* resume_ctx_;

        friend class counting_semaphore;
    };

public:
    explicit counting_semaphore(T desired) noexcept
        : awaiting_(nullptr)
        , counter_(desired) {}

    counting_semaphore(const counting_semaphore&) = delete;
    ~counting_semaphore() noexcept;

    bool try_acquire() noexcept;
    acquire_awaiter acquire() noexcept { return acquire_awaiter{*this}; }

    void release() noexcept;
    void release(T update) noexcept;

private:
    acquire_awaiter* try_release() noexcept;

    std::atomic<acquire_awaiter*> awaiting_;
    acquire_awaiter* to_resume_ = nullptr;
    std::atomic<T> counter_;
    detail::spinlock notifier_mtx_;
};

} // namespace coronet
