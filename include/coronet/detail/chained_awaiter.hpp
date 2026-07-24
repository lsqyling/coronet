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

// ============================================================
// urling_link_io — io_uring 零开销链式等待器
// ============================================================
// 移植自 co_context 的 lazy_link_io 模式。
// 不移动 awaiter 对象，只在第一个 SQE 上设置 IOSQE_IO_LINK 后
// 保存最后一个 awaiter 的指针。内核处理剩余工作。
//
// Port of co_context's lazy_link_io pattern for io_uring:
//   - Zero move, zero copy — only stores a pointer to the last awaiter
//   - Sets IOSQE_IO_LINK on the first SQE (kernel chains the SQEs)
//   - Coroutine handle is set on the last awaiter's task_info
//
// Compared to chained_awaiter (epoll/IOCP):
//   - No refresh_user_data (awaiter not moved)
//   - No chain_fn/chain_target setup (kernel handles linking)
//   - No do_issue_io (SQE was prepared at construction)
template<typename Last>
struct uring_link_io {
    Last* last_io;

    static constexpr bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        last_io->io_info_.handle = h;
    }

    int32_t await_resume() const noexcept {
        return last_io->io_info_.result;
    }
};

// ============================================================
// chained_awaiter — epoll / IOCP 链式等待器
// ============================================================
// 持有两个 I/O awaiter 的副本（通过 move），epoll/IOCP 路径使用。
// io_uring 路径使用上方的 uring_link_io（零 move 开销）。
//
// Holds two I/O awaiters by move (epoll/IOCP path).
// io_uring uses uring_link_io above (zero move overhead).
template<typename First, typename Second>
struct chained_awaiter {
    First first;
    Second second;

    chained_awaiter(First&& f, Second&& s) noexcept
        : first(std::move(f)), second(std::move(s)) {}

    static constexpr bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        // 移动后 io_info_ 地址变化，刷新 user_data
        // After move, io_info_ address changed — refresh user_data
        first.refresh_user_data();
        second.refresh_user_data();

        // 用户协程在第二个操作完成后恢复
        // Coroutine resumes when SECOND operation completes
        second.io_info_.handle = h;

        // 第一个操作完成时，handle_completion 通过 chain_target 找到
        // 第二个 awaiter，调用其内置的 chain_dispatch_fn（per-type
        // CRTP 静态分发，编译期已知完整类型）。
        first.io_info_.chain_target = &second;
        first.io_info_.handle = nullptr;
        first.do_issue_io();  // 启动第一个操作（epoll: 注册 fd；IOCP: 发起 I/O）
    }

    int32_t await_resume() const noexcept {
        return second.io_info_.result;
    }
};

// ---- operator&&：I/O awaitable 的链式 co_await 重载 ----
// 仅匹配具有 do_issue_io() 的类型（I/O awaitable）

namespace impl {
/// io_awaitable concept：有 do_issue_io() 公开方法的类型
/// Concept for types that are I/O awaitables (have do_issue_io()).
template<typename T>
concept io_awaitable = requires(T& t) {
    t.do_issue_io();
};

/// io_uring_awaitable concept：有 sqe_ 成员（io_uring SQE 指针）
template<typename T>
concept uring_awaitable = io_awaitable<T> && requires(T& t) {
    t.sqe_;
    t.sqe_->set_link();  // IOSQE_IO_LINK support
};
} // namespace impl

// ---- io_uring 路径：零开销链式调用（移植自 co_context 的 lazy_link_io） ----
// 不移动 awaiter，只在第一个 SQE 设 IOSQE_IO_LINK，返回仅存指针的 uring_link_io。
// io_uring path: zero-overhead chaining — sets IOSQE_IO_LINK on first SQE
// and returns a lightweight uring_link_io holding only a pointer.
template<impl::uring_awaitable A, impl::uring_awaitable B>
uring_link_io<B> operator&&(A&& a, B&& b) noexcept {
    a.sqe_->set_link();  // 内核级 SQE 链接 / kernel chains SQE[0]→SQE[1]
    a.io_info_.handle = nullptr;  // 跳过第一个 CQE / skip first CQE
    return {&b};
}

// ---- epoll / IOCP 路径：move 两个 awaiter，用户态链式回调 ----
// epoll/IOCP path: move both awaiters, user-space chain dispatch via chain_target.
template<impl::io_awaitable A, impl::io_awaitable B>
chained_awaiter<A, B> operator&&(A&& a, B&& b) noexcept {
    return {std::forward<A>(a), std::forward<B>(b)};
}

} // namespace coronet::detail
