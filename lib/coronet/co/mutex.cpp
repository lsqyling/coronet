#include "coronet/co/mutex.hpp"
#include "coronet/io_context.hpp"

#include <cassert>

namespace coronet {

mutex::~mutex() noexcept {
    [[maybe_unused]] auto state = awaiting_.load(std::memory_order_relaxed);
    assert(state == not_locked || state == locked_no_awaiting);
    assert(to_resume_ == nullptr);
}

bool mutex::try_lock() noexcept {
    auto desire = not_locked;
    return awaiting_.compare_exchange_strong(
        desire, locked_no_awaiting, std::memory_order_acquire,
        std::memory_order_relaxed);
}

void mutex::unlock() noexcept {
    assert(awaiting_.load(std::memory_order_relaxed) != not_locked);
    lock_awaiter* resume_head = to_resume_;
    if (resume_head == nullptr) {
        auto desire = locked_no_awaiting;
        if (awaiting_.compare_exchange_strong(
                desire, not_locked, std::memory_order_release,
                std::memory_order_relaxed)) {
            return;
        }
        auto top = awaiting_.exchange(locked_no_awaiting, std::memory_order_acquire);
        assert(top != not_locked && top != locked_no_awaiting);
        auto* node = std::assume_aligned<alignof(lock_awaiter)>(
            reinterpret_cast<lock_awaiter*>(top));
        do {
            lock_awaiter* tmp = node->next_;
            node->next_ = resume_head;
            resume_head = node;
            node = tmp;
        } while (node != nullptr);
    }
    assert(resume_head != nullptr);
    to_resume_ = resume_head->next_;
    resume_head->co_spawn();
}

bool mutex::lock_awaiter::register_awaiting() noexcept {
    uintptr_t old_state = mtx_.awaiting_.load(std::memory_order_acquire);
    while (true) {
        if (old_state == mutex::not_locked) {
            if (mtx_.awaiting_.compare_exchange_weak(
                    old_state, mutex::locked_no_awaiting,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                return false;
            }
        } else {
            this->next_ = std::assume_aligned<alignof(lock_awaiter)>(
                reinterpret_cast<lock_awaiter*>(old_state));
            if (mtx_.awaiting_.compare_exchange_weak(
                    old_state, reinterpret_cast<uintptr_t>(this),
                    std::memory_order_release, std::memory_order_relaxed)) {
                return true;
            }
        }
    }
}

void mutex::lock_awaiter::co_spawn() const noexcept {
    this->resume_ctx_->spawn_handle(this->awaken_coro_);
}

} // namespace coronet
