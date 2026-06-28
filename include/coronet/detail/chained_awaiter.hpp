#pragma once

#include <coroutine>
#include <utility>

namespace coronet::detail {

/// Chained awaiter: `first` then `second` sequentially.
/// Suspends once; coroutine resumes only after both I/O ops complete.
///
/// Uses task_info's chain_fn/chain_ctx: first op's completion handler
/// auto-starts the second op. Second op's completion resumes user coroutine.
template<typename First, typename Second>
struct chained_awaiter {
    First first;
    Second second;

    chained_awaiter(First&& f, Second&& s) noexcept
        : first(std::move(f)), second(std::move(s)) {}

    static constexpr bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        // After move, io_info_ address changed — update user_data in ops
        first.refresh_user_data();
        second.refresh_user_data();

        // User's coroutine resumes when SECOND completes
        second.io_info_.handle = h;

        if constexpr (requires { first.sqe_; }) {
            // ---- io_uring path: kernel-level SQE linking (IOSQE_IO_LINK) ----
            // Matches co_context's approach — zero user-space overhead.
            first.sqe_->set_link();         // IOSQE_IO_LINK: kernel chains SQE[0]→SQE[1]
            first.io_info_.handle = nullptr; // first CQE is expected (null handle = skip)
            // second's SQE already has user's coroutine handle via io_info_
        } else {
            // ---- IOCP path: user-level chaining via chain_fn ----
            first.io_info_.chain_ctx = &second;
            first.io_info_.chain_fn = [](void* ctx) noexcept {
                static_cast<Second*>(ctx)->do_issue_io();
            };
            first.io_info_.handle = nullptr;
            first.do_issue_io();
        }
    }

    int32_t await_resume() const noexcept {
        return second.io_info_.result;
    }
};

// ---- operator&& : chained co_await for I/O awaitables ----
// Only matches types that have issue_io() and io_info_ (I/O awaitables)

namespace impl {
template<typename T>
concept io_awaitable = requires(T& t) {
    t.do_issue_io();  // public accessor (win_awaiter / io_uring_awaiter)
};
} // namespace impl

template<impl::io_awaitable A, impl::io_awaitable B>
chained_awaiter<A, B> operator&&(A&& a, B&& b) noexcept {
    return {std::forward<A>(a), std::forward<B>(b)};
}

} // namespace coronet::detail
