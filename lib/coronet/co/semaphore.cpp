#include "coronet/co/semaphore.hpp"
#include "coronet/io_context.hpp"

#include <cassert>

namespace coronet {

counting_semaphore::~counting_semaphore() noexcept {
    // No coroutine leak check for simplicity
}

bool counting_semaphore::try_acquire() noexcept {
    T old_counter = counter_.load(std::memory_order_relaxed);
    return old_counter > 0
           && counter_.compare_exchange_strong(
               old_counter, old_counter - 1, std::memory_order_acquire,
               std::memory_order_relaxed);
}

void counting_semaphore::release() noexcept {
    const T update = 1;
    const T old_counter = counter_.fetch_add(update, std::memory_order_release);
    if (old_counter >= 0) return;

    notifier_mtx_.lock();
    acquire_awaiter* awaken_awaiter = try_release();
    notifier_mtx_.unlock();

    if (awaken_awaiter) awaken_awaiter->co_spawn();
}

void counting_semaphore::release(T update) noexcept {
    const T old_counter = counter_.fetch_add(update, std::memory_order_release);
    if (old_counter >= 0) return;

    update = std::max(old_counter, -update);
    {
        notifier_mtx_.lock();
        do {
            acquire_awaiter* awaken_awaiter = try_release();
            if (awaken_awaiter) awaken_awaiter->co_spawn();
        } while (++update < 0);
        notifier_mtx_.unlock();
    }
}

counting_semaphore::acquire_awaiter*
counting_semaphore::try_release() noexcept {
    acquire_awaiter* resume_head = to_resume_;
    if (resume_head == nullptr) [[unlikely]] {
        auto* node = awaiting_.exchange(nullptr, std::memory_order_acquire);
        if (node == nullptr) [[unlikely]]
            return nullptr;

        do {
            acquire_awaiter* tmp = node->next_;
            node->next_ = resume_head;
            resume_head = node;
            node = tmp;
        } while (node != nullptr);
    }

    assert(resume_head != nullptr);
    to_resume_ = resume_head->next_;
    return resume_head;
}

void counting_semaphore::acquire_awaiter::await_suspend(
    std::coroutine_handle<> current) noexcept
{
    this->handle_ = current;
    acquire_awaiter* old_head = sem_.awaiting_.load(std::memory_order_relaxed);
    do {
        this->next_ = old_head;
    } while (!sem_.awaiting_.compare_exchange_weak(
        old_head, this, std::memory_order_release, std::memory_order_relaxed));
}

void counting_semaphore::acquire_awaiter::co_spawn() const noexcept {
    this->resume_ctx_->spawn_handle(this->handle_);
}

} // namespace coronet
