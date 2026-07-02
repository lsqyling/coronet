#pragma once

#ifdef __GNUC__
#pragma GCC push_macro("linux")
#undef linux
#endif

#include "coronet/detail/task_info.hpp"
#include "coronet/detail/thread_meta.hpp"
#include "coronet/detail/worker_meta.hpp"
#include "coronet/detail/user_data.hpp"
#include "coronet/platform/io_uring/io_uring_proactor.hpp"

#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <span>

namespace coronet::detail {

/// Base class for Linux io_uring awaitables.
/// Gets an SQE directly from the io_uring proactor (no heap alloc for wrapper).
// io_uring awaiter 的基类。
// 直接从 proactor 获取 SQE（无需为包装而堆分配）。
//
// 关键设计决策：
//   - io_uring 是 completion-based（基于完成）模型，因此 awaiter 在构造函数中就准备好 SQE，
//     在 await_suspend 中只需保存协程 handle，无需执行任何 syscall。
//   - 这与 epoll 的 readiness-based（基于就绪）模型形成对比：
//     epoll 在 await_suspend 中将 fd 注册到 epoll，在 epoll_wait 返回后执行实际的 I/O syscall。
//     io_uring 则在构造函数中就通过 get_sq_entry 拿到了 SQE 并填充好了 I/O 参数，
//     后续由 submit() 统一批量提交到内核。
//   - SQE 直接从 proactor 的 SQ ring 中分配，零堆分配开销。
//   - 所有派生类型（recv/send/accept/connect/close/shutdown/read/write/openat/nop/yield/timeout）
//     都继承这个基类，通过不同的 prep_* 调用填充不同的 I/O 操作。
//
// 为什么没有用 CRTP？
//   - io_uring 的 awaiter 不需要 CRTP，因为所有 I/O 参数都在构造时通过 SQE 准备好，
//     不需要派生类提供自定义的 register_with_epoll 或 perform 回调。
//   - 行为差异通过不同的 prep_* 方法在构造函数中确定，这在编译期就已经完全确定。
class io_uring_awaiter {
public:
    [[nodiscard]] int32_t result() const noexcept { return io_info_.result; }
    static constexpr bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> current) noexcept {
        io_info_.handle = current;
        // await_suspend 只保存协程 handle，不涉及 syscall。
        // I/O 请求已经在构造函数中通过 SQE 准备好，后续由 submit() 批量提交。
        // 这种"构造时准备、统一提交"的模式减少了用户态/内核态切换次数。
    }

    [[nodiscard]] int32_t await_resume() const noexcept { return result(); }
    [[nodiscard]] uint64_t user_data() const noexcept {
        return sqe_->get_data();
    }

    // Public for chained co_await
    // 公开给 chained_awaiter（operator&&）使用，用于链式 co_await
    void do_issue_io() noexcept {}  // io_uring: SQE already prepared
    // io_uring 的 SQE 已在构造函数中准备好，do_issue_io 为空操作
    void refresh_user_data() noexcept {
        sqe_->set_data(io_info_.as_user_data() | uint64_t(user_data_type::task_info_ptr));
    }

protected:
    io_uring_awaiter() noexcept {
        auto* p = static_cast<platform::io_uring::io_uring_proactor*>(
            this_thread.worker->proactor);
        // 直接从 proactor 获取 SQE，零堆分配
        sqe_ = p->get_sq_entry();
        sqe_->set_data(
            io_info_.as_user_data()
            | uint64_t(detail::user_data_type::task_info_ptr));
        // Track the inflight operation count
        // 追踪飞行中的操作计数
        ++this_thread.worker->requests_to_reap;
        ++this_thread.worker->requests_to_submit;
    }

public:
    liburingcxx::sq_entry* sqe_ = nullptr;
    // Public: accessed by chained_awaiter (operator&&)
    // 公开成员，供 chained_awaiter（operator&&）访问
    task_info io_info_;
};

// ============================================================
// Socket I/O
// ============================================================

// 套接字 I/O：每个 awaiter 类型对应一种 io_uring 操作
// 在构造函数中调用对应的 prep_* 方法填充 SQE
// 所有操作都是无堆分配的：SQE 预分配在 SQ ring 中，awaiter 在栈上创建

struct io_uring_recv : io_uring_awaiter {
    io_uring_recv(int fd, std::span<char> buf, int flags = 0) noexcept
        : io_uring_awaiter() { sqe_->prep_recv(fd, buf, flags); }
};

struct io_uring_send : io_uring_awaiter {
    io_uring_send(int fd, std::span<const char> buf, int flags = 0) noexcept
        : io_uring_awaiter() { sqe_->prep_send(fd, buf, flags); }
};

struct io_uring_accept : io_uring_awaiter {
    io_uring_accept(int fd, struct sockaddr* addr = nullptr,
                    socklen_t* addrlen = nullptr, int flags = 0) noexcept
        : io_uring_awaiter() { sqe_->prep_accept(fd, addr, addrlen, flags); }
};

struct io_uring_connect : io_uring_awaiter {
    io_uring_connect(int fd, const struct sockaddr* addr, socklen_t addrlen) noexcept
        : io_uring_awaiter() { sqe_->prep_connect(fd, addr, addrlen); }
};

struct io_uring_close : io_uring_awaiter {
    explicit io_uring_close(int fd) noexcept
        : io_uring_awaiter() { sqe_->prep_close(fd); }
};

struct io_uring_shutdown : io_uring_awaiter {
    io_uring_shutdown(int fd, int how) noexcept
        : io_uring_awaiter() { sqe_->prep_shutdown(fd, how); }
};

// ============================================================
// File I/O
// ============================================================

// 文件 I/O：与套接字 I/O 使用相同的接口模式
// io_uring 支持文件系统的异步 I/O（pread/pwrite 等），这也是它优于 epoll 的重要特性之一
// epoll 不支持普通文件的就绪通知（普通文件总是被认为"就绪"），需要后台线程模拟异步
// io_uring 则由内核直接完成文件 I/O，无需额外线程

struct io_uring_read : io_uring_awaiter {
    io_uring_read(int fd, std::span<char> buf, uint64_t offset = uint64_t(-1)) noexcept
        : io_uring_awaiter() { sqe_->prep_read(fd, buf, offset); }
    // offset = uint64_t(-1) 表示使用文件当前位置（相当于 read 而非 pread）
};

struct io_uring_write : io_uring_awaiter {
    io_uring_write(int fd, std::span<const char> buf, uint64_t offset = uint64_t(-1)) noexcept
        : io_uring_awaiter() { sqe_->prep_write(fd, buf, offset); }
};

struct io_uring_openat : io_uring_awaiter {
    io_uring_openat(int dirfd, const char* path, int flags, mode_t mode = 0) noexcept
        : io_uring_awaiter() { sqe_->prep_openat(dirfd, path, flags, mode); }
};

// ============================================================
// Control / timer / yield
// ============================================================

// 控制类操作：NOP、yield 和时间等待
// NOP 和 yield 都使用 prep_nop — NOP 表示空操作，yield 用于让出执行权给其他协程

struct io_uring_nop : io_uring_awaiter {
    io_uring_nop() noexcept : io_uring_awaiter() { sqe_->prep_nop(); }
};

struct io_uring_yield : io_uring_awaiter {
    io_uring_yield() noexcept : io_uring_awaiter() { sqe_->prep_nop(); }
};

struct io_uring_timeout : io_uring_awaiter {
  private:
    __kernel_timespec ts_{};  // MUST be member: SQE stores pointer to this
    // 必须作为成员变量：SQE 中存储的是指向 timespec 的指针
    // 如果 ts_ 在栈上创建，在 SQE 被内核处理时可能已经销毁
    // 因此 ts_ 必须是 awaiter 的成员变量，保证其生命周期覆盖整个 I/O 操作

  public:
    template<typename Rep, typename Period>
    explicit io_uring_timeout(const std::chrono::duration<Rep, Period>& dur) noexcept
        : io_uring_awaiter() {
        // 将 duration 转换为内核需要的 timespec 结构
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
        ts_.tv_sec  = static_cast<decltype(ts_.tv_sec)>(ns / 1'000'000'000LL);
        ts_.tv_nsec = static_cast<decltype(ts_.tv_nsec)>(ns % 1'000'000'000LL);
        // co_context alignment: timeout_relative_flag | pure_timer_flag
        // timeout_flags 中的 IORING_TIMEOUT_ABS 或相关标志控制超时是绝对时间还是相对时间
        sqe_->prep_timeout(ts_, 0, config::timeout_flags);
    }
};

} // namespace coronet::detail

// Platform factory functions — uniform interface for async_io.hpp
// 平台工厂函数 — 为 async_io.hpp 提供统一接口
// 这些 inline 函数通过平台命名空间 platform_io 导出，async_io.hpp 通过 make_* 调用它们。
// 不同平台（io_uring/iocp/epoll）各自定义自己的 platform_io 命名空间，
// 通过 #ifdef 在编译期选择具体实现，无需运行时多态。
namespace coronet::detail::platform_io {
    inline auto make_recv(int fd, std::span<char> buf, int flags) noexcept
        { return io_uring_recv{fd, buf, flags}; }
    inline auto make_send(int fd, std::span<const char> buf, int flags) noexcept
        { return io_uring_send{fd, buf, flags}; }
    inline auto make_accept(int fd, struct sockaddr* a, socklen_t* al, int fl) noexcept
        { return io_uring_accept{fd, a, al, fl}; }
    inline auto make_connect(int fd, const struct sockaddr* a, socklen_t al) noexcept
        { return io_uring_connect{fd, a, al}; }
    inline auto make_close(int fd) noexcept
        { return io_uring_close{fd}; }
    inline auto make_shutdown(int fd, int how) noexcept
        { return io_uring_shutdown{fd, how}; }
    inline auto make_read(int fd, std::span<char> buf, uint64_t off) noexcept
        { return io_uring_read{fd, buf, off}; }
    inline auto make_write(int fd, std::span<const char> buf, uint64_t off) noexcept
        { return io_uring_write{fd, buf, off}; }
    inline auto make_yield() noexcept
        { return io_uring_yield{}; }
    template<typename D>
    inline auto make_timeout(D dur) noexcept
        { return io_uring_timeout{dur}; }
} // namespace coronet::detail::platform_io

#ifdef __GNUC__
#pragma GCC pop_macro("linux")
#endif
