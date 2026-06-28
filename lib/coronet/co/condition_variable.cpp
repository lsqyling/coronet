#include "coronet/co/condition_variable.hpp"
#include "coronet/io_context.hpp"

#include <utility>

namespace coronet::detail {

void cv_wait_awaiter::await_suspend(std::coroutine_handle<> current) noexcept {
    this->lock_awaken_handle_.register_coroutine(current);

    cv_wait_awaiter* old_head = cv_.awaiting_.load(std::memory_order_relaxed);
    do {
        this->next_ = old_head;
    } while (!cv_.awaiting_.compare_exchange_weak(
        old_head, this, std::memory_order_release, std::memory_order_relaxed));

    this->lock_awaken_handle_.unlock_ahead();
}

} // namespace coronet::detail

namespace coronet {

void condition_variable::notify_one() noexcept {
    auto try_notify_one = [](cv_wait_awaiter* head) {
        auto& awaken_awaiter = head->lock_awaken_handle_;
        if (!awaken_awaiter.register_awaiting()) [[unlikely]] {
            awaken_awaiter.co_spawn();
        }
    };

    cv_wait_awaiter* head;

    notifier_mtx_.lock();
    head = to_resume_head_;
    if (to_resume_head_ != nullptr) {
        to_resume_head_ = to_resume_head_->next_;
        notifier_mtx_.unlock();
        try_notify_one(head);
        return;
    }
    to_resume_tail_ = nullptr;
    notifier_mtx_.unlock();

    head = awaiting_.exchange(nullptr, std::memory_order_acquire);
    if (head == nullptr) [[unlikely]]
        return;

    cv_wait_awaiter* const tail = head;
    cv_wait_awaiter* succ = nullptr;
    cv_wait_awaiter* pred = head->next_;
    while (pred != nullptr) {
        head->next_ = succ;
        succ = head;
        head = pred;
        pred = pred->next_;
    }

    notifier_mtx_.lock();
    if (to_resume_head_ == nullptr) [[likely]] {
        to_resume_head_ = succ;
        to_resume_tail_ = tail;
        notifier_mtx_.unlock();
        try_notify_one(head);
        return;
    }
    to_resume_tail_->next_ = head;
    cv_wait_awaiter* const to_notify = to_resume_head_;
    to_resume_head_ = to_resume_head_->next_;
    to_resume_tail_ = tail;
    notifier_mtx_.unlock();
    try_notify_one(to_notify);
}

void condition_variable::notify_all() noexcept {
    auto try_notify_all = [](cv_wait_awaiter* head) {
        while (head != nullptr) {
            auto& awaken_awaiter = head->lock_awaken_handle_;
            if (!awaken_awaiter.register_awaiting()) [[unlikely]] {
                awaken_awaiter.co_spawn();
            }
            head = head->next_;
        }
    };

    cv_wait_awaiter* resume_head =
        awaiting_.exchange(nullptr, std::memory_order_acquire);
    try_notify_all(resume_head);

    notifier_mtx_.lock();
    resume_head = to_resume_head_;
    to_resume_head_ = nullptr;
    notifier_mtx_.unlock();
    try_notify_all(resume_head);
}

} // namespace coronet
