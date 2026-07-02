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

/*
 * 链式等待器：持有两个 I/O 操作，依次执行。
 *
 * 为什么需要链式 I/O：
 * - 在异步网络中，"先接收请求，再发送回复"是一个常见模式
 * - 如果分两次 co_await（先 co_await recv，再 co_await send），
 *   协程需要挂起两次，每次都涉及完整的调度循环
 * - 链式 I/O 将两个操作合并为一次挂起，减少了一半的协程恢复开销
 * - 在 io_uring 上，链式操作还可以利用内核的 SQE 链接优化，
 *   减少系统调用次数（一次 io_uring_enter 提交两个 SQE，
 *   且内核保证它们按序执行）
 *
 * 双路径实现：
 * 1. io_uring 路径（编译期检测到 sqe_ 成员）：
 *    - 对第一个 SQE 设置 IOSQE_IO_LINK 标志
 *    - 第一个 CQE 被内核自动处理（不唤醒用户协程）
 *    - 第二个 CQE 到达时恢复用户协程
 *    - 零用户空间开销 —— 所有串联逻辑由内核完成
 *
 * 2. epoll/IOCP 路径（无 sqe_ 成员）：
 *    - 第一个操作的 chain_fn 被设置为启动第二个操作的回调
 *    - 当第一个操作完成时，handle_completion 检测到 chain_fn
 *      非空，执行 chain_fn 启动第二个操作
 *    - 第二个操作完成时，恢复用户协程（因为 handle 设置在第二个操作上）
 *    - 这种方式有一个额外的函数指针调用开销，但避免了再次 co_await
 *
 * 关键设计点：
 * - 用户协程句柄始终挂在第二个操作上，这是语义上的正确选择：
 *   用户希望"两个操作都完成"时才恢复
 * - 第一个操作的结果被链式逻辑消费（对应用透明），
 *   await_resume 只返回第二个操作的结果
 * - refresh_user_data() 在移动后必须调用，因为 task_info 对象的地址
 *   在移动后发生了变化，需要更新 user_data 指向新的地址
 */
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
    /// Returns the result of the second operation (first op's result is consumed by chain logic).
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
    t.do_issue_io();  // public accessor (win_awaiter / io_uring_awaiter / epoll_awaiter_base)
};
} // namespace impl

/// 重载 operator&&，返回 chained_awaiter
/// Overload operator&& for I/O awaitables — returns a chained_awaiter.
/*
 * operator&& 的重载不是常规的"逻辑与"语义，而是"顺序组合"。
 * 选择 && 符号而非其他操作符的原因：
 * - && 是 C++ 中唯一表示"序列点"语义的内置操作符
 * - a && b 意味着"如果 a 成功了，再执行 b"——这与链式 I/O 的语义一致：
 *   "先完成 a，再完成 b"
 * - 这借鉴了 co_context 库的设计选择，保持了社区的一致性
 *
 * 约束：两个参数必须都是 I/O awaitable（有 do_issue_io() 方法）。
 * 返回 chained_awaiter 对象，该对象本身也是一个 awaitable。
 */
template<impl::io_awaitable A, impl::io_awaitable B>
chained_awaiter<A, B> operator&&(A&& a, B&& b) noexcept {
    return {std::forward<A>(a), std::forward<B>(b)};
}

} // namespace coronet::detail
