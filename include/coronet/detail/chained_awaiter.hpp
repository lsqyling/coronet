#pragma once
// ============================================================
// chained_awaiter.hpp — 链式 co_await（operator&&）
// ============================================================
// 语法: co_await (recv(fd, buf) && send(fd, reply))
// 协程只挂起一次，两个 I/O 操作依次完成后才恢复。
//
// 双路径实现（编译期选择）：
//   - io_uring：内核级 SQE 链接 (IOSQE_IO_LINK)，零 userspace 开销
//   - epoll/IOCP：用户态链式回调 chain_fn，第一个完成时自动启动第二个
//
// Chained awaiter: `first` then `second` sequentially.
// Suspends once; coroutine resumes only after both I/O ops complete.

#include <coroutine>
#include <utility>

namespace coronet::detail {

/// 链式等待器：持有两个 I/O awaiter，依次执行。
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
        // 移动后 io_info_ 地址变化，刷新 user_data / After move, refresh user_data
        first.refresh_user_data();
        second.refresh_user_data();

        // 用户协程在第二个操作完成后恢复 / User's coroutine resumes when SECOND completes
        second.io_info_.handle = h;

        // 编译期判断 io_uring 路径（通过 sqe_ 成员检测）
        if constexpr (requires { first.sqe_; }) {
            // ---- io_uring 路径：内核级 SQE 链接 (IOSQE_IO_LINK) ----
            // 与 co_context 的设计对齐 — 零 userspace 开销
            // Matches co_context's approach — zero user-space overhead.
            first.sqe_->set_link();          // IOSQE_IO_LINK: 内核串联 SQE[0]→SQE[1]
            first.io_info_.handle = nullptr; // 第一个 CQE 被跳过（由内核处理）
            // 第二个 SQE 已通过 io_info_ 持有用户协程句柄
        } else {
            // ---- epoll / IOCP 路径：用户态链式回调 chain_fn ----
            // 第一个操作完成时，handle_completion 调用 chain_fn 启动第二个操作
            first.io_info_.chain_ctx = &second;
            first.io_info_.chain_fn = [](void* ctx) noexcept {
                static_cast<Second*>(ctx)->do_issue_io();
            };
            first.io_info_.handle = nullptr;
            first.do_issue_io();  // 启动第一个操作（epoll: 注册 fd；IOCP: 发起 I/O）
        }
    }

    /// 返回第二个操作的结果（第一个操作的结果被链式逻辑消费）
    int32_t await_resume() const noexcept {
        return second.io_info_.result;
    }
};

// ---- operator&&：I/O awaitable 的链式 co_await 重载 ----
// 仅匹配具有 do_issue_io() 的类型（I/O awaitable）

namespace impl {
/// io_awaitable concept：有 do_issue_io() 公开方法的类型
template<typename T>
concept io_awaitable = requires(T& t) {
    t.do_issue_io();  // public accessor (win_awaiter / io_uring_awaiter / epoll_awaiter_base)
};
} // namespace impl

/// 重载 operator&&，返回 chained_awaiter
/// Overload operator&& for I/O awaitables — returns a chained_awaiter.
template<impl::io_awaitable A, impl::io_awaitable B>
chained_awaiter<A, B> operator&&(A&& a, B&& b) noexcept {
    return {std::forward<A>(a), std::forward<B>(b)};
}

} // namespace coronet::detail
