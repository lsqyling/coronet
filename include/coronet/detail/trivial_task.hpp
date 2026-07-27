#pragma once

#include <coroutine>
#include "coronet/detail/thread_meta.hpp"

namespace coronet::detail {

/*
 * 最小化的协程类型 —— 专用于 condition_variable 的谓词等待。
 *
 * 为什么需要这个类型：
 * - condition_variable::wait(lock, predicate) 需要在等待期间多次执行
 *   谓词函数（每次被 notify 时检查条件是否满足）
 * - 在协程环境中，我们需要一个轻量级的协程来包装这个"检查-等待"循环
 * - 使用完整的 task<T> 太重了（task 支持返回值、异常传播、链式操作等），
 *   而 trivial_task 只需要"执行、暂停、销毁"三个操作
 *
 * 设计特点：
 * - 不支持返回值（return_void）
 * - 不传播异常（unhandled_exception 为空）
 * - 初始挂起（initial_suspend 返回 suspend_always），等用户显式启动
 * - 最终挂起时自动销毁协程帧并恢复父协程（final_suspend 的 await_suspend
 *   调用 current.destroy() 后返回 parent_coro）
 *
 * 生命周期：
 * 调用方创建 trivial_task -> 获取其 handle ->
 * 在 condition_variable 等待循环中 -> co_await 此 trivial_task ->
 * trivial_task 执行谓词检查 -> 如果条件不满足，trivial_task 挂起 ->
 * 被 notify 时恢复 -> 再次检查 -> ... -> 条件满足时正常结束 ->
 * final_suspend 销毁协程帧并恢复原始协程
 *
 * 这个协程在 condition_variable 的内部链表中作为一个"节点"存在。
 * 因为它是协程，可以自然地挂起和恢复，无需额外的状态机或回调管理。
 */
/// Minimal coroutine type for condition_variable's predicate wait.
/// 专用于条件变量的谓词等待的最小协程类型。
class trivial_task {
public:
    struct promise_type;

    explicit trivial_task(std::coroutine_handle<promise_type> self_handle) noexcept
        : handle(self_handle) {}

    // P0-1 fix: 析构时销毁协程帧。
    // final_awaiter::await_suspend 返回 void，将帧留在 final suspend 点。
    // 此时 handle.done() == true，调用 destroy() 安全。
    // 若协程从未启动（initial_suspend 挂起），destroy() 也安全。
    //
    // Destroy the coroutine frame on destruction.
    // final_awaiter returns void, leaving the frame at final-suspend point.
    // handle.done() == true at that point, so destroy() is safe.
    // If the coroutine was never started (suspended at initial_suspend),
    // destroy() is also safe.
    ~trivial_task() {
        if (handle) handle.destroy();
    }

    // Move semantics: nullify source to prevent double-destroy
    // 移动语义：源对象置空，防止双重销毁
    trivial_task(trivial_task&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }
    trivial_task& operator=(trivial_task&&) = delete;  // no assignment needed

    trivial_task(const trivial_task&) = delete;
    trivial_task& operator=(const trivial_task&) = delete;

    /*
     * final_awaiter：协程即将结束时执行的操作。
     * await_suspend 的逻辑：
     * 1. 获取父协程句柄（由 await_suspend 阶段设置）
     * 2. 销毁当前协程帧（协程已执行完毕，不再需要）
     * 3. 恢复父协程（让等待在 trivial_task 上的协程继续执行）
     *
     * 为什么在 final_suspend 中销毁协程帧：
     * - 协程帧的析构必须在协程挂起后进行（不能由外部销毁）
     * - 在 final_suspend 的 await_suspend 中，当前协程即将结束，
     *   可以安全地销毁其帧
     * - 使用 current.destroy() 而非让调用方管理，消除了忘记销毁的风险
     *
     * 与 condition_variable 的交互：
     * - 当条件满足时，谓词协程正常执行完所有代码，进入 final_suspend
     * - final_suspend 销毁帧并恢复原始协程（持有锁的那个）
     * - 原始协程继续执行，检查到条件满足，释放锁
     */
    struct final_awaiter {
        // 不在 await_suspend 中调用 current.destroy()。
        // MSVC 协程运行时在 await_suspend 返回后可能访问协程帧，
        // 导致 use-after-free segfault。
        // 改为将父协程通过 spawn_handle 调度恢复，返回 void 让
        // 运行时标记当前协程挂起（帧保持存活，由调用方后续销毁）。
        //
        // Do NOT call current.destroy() inside await_suspend.
        // MSVC's coroutine runtime may access the frame after
        // await_suspend returns, causing use-after-free.
        // Instead, schedule the parent for resumption via the
        // io_context and return void so the runtime leaves the
        // frame suspended (it will be cleaned up by the caller).
        static constexpr bool await_ready() noexcept { return false; }

        static void
        await_suspend(std::coroutine_handle<promise_type> current) noexcept {
            auto continuation = current.promise().parent_coro;
            // 调度父协程恢复：通过 thread_meta 的自由函数，避免 include
            // worker_meta.hpp（trivial_task 保持轻量，不拖入调度器完整定义）。
            // 不在 await_suspend 中调用 current.destroy()，以避免 MSVC 运行时
            // 在 await_suspend 返回后访问已释放帧。
            //
            // Schedule parent resumption via the thread_meta helper so
            // trivial_task need not include worker_meta.hpp. Do NOT call
            // current.destroy() here — MSVC's runtime may access the
            // frame after await_suspend returns.
            coronet::detail::schedule_on_this_thread(continuation);
            // 帧在 final suspend 点保持存活，由 trivial_task 析构函数销毁。
            // Frame stays alive at final-suspend point, destroyed by
            // trivial_task's destructor (which calls handle.destroy()).
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

    /*
     * 当调用方 co_await trivial_task 时触发：
     * - 将调用方的协程句柄保存到 promise.parent_coro 中
     * - 返回 trivial_task 自身的协程句柄，使执行跳转到此 trivial_task
     *
     * 协程跳转链：
     * 调用方协程 -> await_suspend -> 返回 handle ->
     * 执行权转移到 trivial_task -> trivial_task 执行谓词 ->
     * 谓词返回 false -> trivial_task 挂起（在 condition_variable 的等待链中） ->
     * ... 被 notify 恢复 ... -> 谓词返回 false（继续挂起）
     * 或 -> 谓词返回 true -> trivial_task 正常结束 ->
     * final_suspend -> 销毁帧 -> 恢复调用方协程
     */
    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> awaiting_coro) const noexcept {
        handle.promise().parent_coro = awaiting_coro;
        return handle;
    }

    static constexpr void await_resume() noexcept {}

    std::coroutine_handle<promise_type> handle;
};

} // namespace coronet::detail
