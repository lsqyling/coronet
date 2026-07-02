#pragma once

#include "coronet/detail/task_info.hpp"
#include "coronet/detail/thread_meta.hpp"
#include "coronet/detail/worker_meta.hpp"
#include "coronet/detail/user_data.hpp"
#include "coronet/platform/epoll/epoll_reactor.hpp"

#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <span>

#include <cerrno>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

// O_NONBLOCK (04000) | O_CLOEXEC (02000000) — avoid <fcntl.h> to prevent
// struct stat from shadowing ::stat() in downstream translation units.
// 使用硬编码的标志值而非包含 <fcntl.h>，避免 struct stat 与 ::stat() 冲突。
// O_NONBLOCK = 04000（八进制），O_CLOEXEC = 02000000（八进制）
// 这是 Linux 上避免头文件冲突的常见技巧。
namespace {
constexpr int kPipe2Flags = 04000 | 02000000;
}

namespace coronet::detail {

// ============================================================
// epoll_awaiter_base<Derived> — CRTP base for epoll awaitables
// ============================================================
// epoll_awaiter_base<Derived> — epoll awaiter 的 CRTP 基类
//
// Replaces virtual dispatch with compile-time CRTP:
//   - await_suspend / do_issue_io dispatch to Derived::register_with_epoll()
//     and Derived::perform_sync_op() with zero runtime overhead (static_cast).
//   - Default implementations for register_with_epoll() and perform_sync_op()
//     live in the base; derived types override by name-hiding.
//   - The perform callback (op_ctx_.perform) is already a static function
//     pointer — no change needed there.
//
// 用编译期 CRTP 替代虚函数派发：
//   - await_suspend / do_issue_io 派发到 Derived::register_with_epoll()
//     和 Derived::perform_sync_op()，通过 static_cast 实现零运行时开销。
//   - register_with_epoll() 和 perform_sync_op() 的默认实现在基类中，
//     派生类通过 name-hiding（名称隐藏）覆盖。
//   - perform 回调（op_ctx_.perform）本身已经是静态函数指针，无需修改。
//
// 为什么使用 CRTP 而非虚函数？
//   - CRTP 在编译期确定所有调用目标，编译器可以完全内联，
//     没有 vtable 查找和间接分支的开销。
//   - epoll 的 readiness-based 模型需要区分异步操作（走 epoll）和同步操作（立即完成），
//     两者在 await_suspend 中的行为完全不同，CRTP 使得这种区分在编译期完成。
//   - 虚函数版本：每个 awaiter 都有一个 vtable 指针（8 字节），
//     在协程的栈帧上可能造成额外的缓存压力。
//
// 异步 vs 同步操作的设计：
//   - 异步操作（socket I/O）：需要注册到 epoll，等待 fd 就绪后执行 I/O syscall。
//     构造函数使用两个参数的版本，设置 is_async_ = true。
//   - 同步操作（close, shutdown, nop, yield）：不经过 epoll，在 await_suspend 中直接执行。
//     构造函数使用一个参数的版本，设置 is_async_ = false。
//   - 同步操作不需要 requests_to_reap 递增（因为没有 CQE/完成事件等待）。
//     在 await_suspend 中直接执行 perform_sync_op() 并通过 forward_task 恢复协程。
//
// 管道信号机制（用于文件 I/O）：
//   普通文件在 epoll 中总是被认为是"就绪"的（always ready），因为文件 I/O 不会阻塞。
//   因此 epoll 无法用于检测文件 I/O 的完成。解决方案是使用 pipe + 后台线程：
//     - 后台线程执行实际的文件 I/O（read/write）
//     - 完成后向 pipe 写入一个字节
//     - 前台线程注册 pipe 读端到 epoll，pipe 可读时表示文件 I/O 已完成
//   epoll_read 和 epoll_write 通过覆盖 register_with_epoll() 实现此模式。

template<typename Derived>
class epoll_awaiter_base {
public:
    [[nodiscard]] int32_t result() const noexcept { return io_info_.result; }
    static constexpr bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> current) noexcept {
        io_info_.handle = current;
        if (is_async_) {
            // 异步操作：通过 CRTP 调用派生类的 register_with_epoll() 将 fd 注册到 epoll。
            // 注册完成后，协程暂停等待 epoll_wait 返回就绪事件。
            static_cast<Derived*>(this)->register_with_epoll();
        } else {
            // Synchronous: complete immediately without epoll
            // 同步操作：不经过 epoll，直接执行 I/O 操作并立即恢复协程。
            io_info_.result = static_cast<Derived*>(this)->perform_sync_op();
            // requests_to_reap was NOT incremented for sync ops
            // 同步操作没有递增 requests_to_reap
            this_thread.worker->forward_task(current);
            // 通过 forward_task 将协程放回执行队列，使其在下次调度时恢复
            io_info_.handle = nullptr;
        }
    }

    [[nodiscard]] int32_t await_resume() const noexcept { return result(); }

    [[nodiscard]] uint64_t user_data() const noexcept {
        return io_info_.as_user_data();
    }

    // Public for chained co_await — dispatches via CRTP
    // 公开给 chained_awaiter（operator&&）使用 — 通过 CRTP 派发
    void do_issue_io() noexcept {
        if (is_async_) {
            static_cast<Derived*>(this)->register_with_epoll();
        }
    }

    void refresh_user_data() noexcept {
        op_ctx_.user_data = io_info_.as_user_data()
            | uint64_t(user_data_type::task_info_ptr);
    }

protected:
    /// Constructor for async ops (go through epoll).
    // 异步操作的构造函数（经过 epoll）。
    epoll_awaiter_base(int fd, uint32_t epoll_events) noexcept
        : fd_(fd), epoll_events_(epoll_events), is_async_(true) {
        op_ctx_.self = static_cast<Derived*>(this);
        op_ctx_.fd = fd;
        op_ctx_.user_data = io_info_.as_user_data()
            | uint64_t(user_data_type::task_info_ptr);
        // 异步操作需要递增 requests_to_reap，
        // 这样事件循环知道还有未完成的 I/O 操作在等待。
        ++this_thread.worker->requests_to_reap;
    }

    /// Constructor for sync ops (complete immediately, no epoll).
    // 同步操作的构造函数（立即完成，不经过 epoll）。
    explicit epoll_awaiter_base(int fd) noexcept
        : fd_(fd), epoll_events_(0), is_async_(false) {
        op_ctx_.fd = fd;
    }

    // ---- Default implementations (overridden by name-hiding in derived) ----

    /// Default epoll registration. Override in async derived types that need
    /// custom setup (e.g. epoll_read / epoll_write create a pipe first).
    // 默认的 epoll 注册逻辑。派生类可覆盖以实现自定义设置（如文件 I/O 先创建 pipe）。
    void register_with_epoll() noexcept {
        auto* p = static_cast<platform::epoll::epoll_proactor*>(
            this_thread.worker->proactor);
        p->register_fd(fd_, epoll_events_, &op_ctx_);
    }

    /// Default sync operation (no-op). Override in sync derived types.
    // 默认的同步操作（空操作）。同步派生类通过 name-hiding 覆盖。
    int32_t perform_sync_op() noexcept { return 0; }

public:
    // Public: accessed by chained_awaiter (operator&&)
    // 公开成员：供 chained_awaiter（operator&&）访问
    task_info io_info_;

    // Completion context — stored in epoll_event.data.ptr
    // 完成上下文 — 存储在 epoll_event.data.ptr 中
    platform::epoll::epoll_completion_ctx op_ctx_{};

protected:
    int fd_;
    uint32_t epoll_events_;
    bool is_async_;
};

// ============================================================
// Helper: perform a non-blocking I/O syscall, return result
// ============================================================
// 辅助函数：执行非阻塞 I/O syscall，返回结果
// 将 posix 系统调用的返回值（ssize_t）转换为统一的 int32_t 格式：
//   - 非负值：成功，表示传输的字节数
//   - 负值：失败，-errno 表示具体的错误码
namespace {

inline int32_t sys_result(ssize_t n) noexcept {
    return (n >= 0) ? static_cast<int32_t>(n) : -errno;
}

} // anonymous namespace

// ============================================================
// Socket I/O — async (epoll-based, default register_with_epoll)
// ============================================================
// Socket I/O — 异步（基于 epoll，使用默认的 register_with_epoll）
//
// 每个 awaiter 结构体：
//   1. 继承 epoll_awaiter_base<自身>（CRTP）
//   2. 在构造函数中调用基类的异步构造函数（传入 fd 和 epoll 事件类型 EPOLLIN/EPOLLOUT）
//   3. 设置 op_ctx_.perform 为静态的 do_perform 函数
//   4. do_perform 在 epoll_wait 返回后被调用，执行实际的 I/O syscall
//
// perform 静态函数的设计：
//   - 签名 int(int fd, void* self) 是统一的，不管实际 I/O 类型是什么
//   - 内部将 self 转换回具体的 awaiter 类型以访问其成员
//   - 将结果存入 io_info_.result，稍后 await_resume() 返回

struct epoll_recv final : epoll_awaiter_base<epoll_recv> {
    epoll_recv(int fd, std::span<char> buf, int flags = 0) noexcept
        : epoll_awaiter_base(fd, EPOLLIN), buf_(buf), flags_(flags)
        // EPOLLIN：监听 fd 的可读事件
    {
        op_ctx_.perform = &epoll_recv::do_perform;
    }

private:
    std::span<char> buf_;
    int flags_;

    static int do_perform(int fd, void* self) noexcept {
        auto* a = static_cast<epoll_recv*>(self);
        a->io_info_.result = sys_result(
            ::recv(fd, a->buf_.data(), a->buf_.size(), a->flags_));
        return a->io_info_.result;
    }
};

struct epoll_send final : epoll_awaiter_base<epoll_send> {
    epoll_send(int fd, std::span<const char> buf, int flags = 0) noexcept
        : epoll_awaiter_base(fd, EPOLLOUT), buf_(buf), flags_(flags)
        // EPOLLOUT：监听 fd 的可写事件
    {
        op_ctx_.perform = &epoll_send::do_perform;
    }

private:
    std::span<const char> buf_;
    int flags_;

    static int do_perform(int fd, void* self) noexcept {
        auto* a = static_cast<epoll_send*>(self);
        a->io_info_.result = sys_result(
            ::send(fd, a->buf_.data(), a->buf_.size(), a->flags_));
        return a->io_info_.result;
    }
};

struct epoll_accept final : epoll_awaiter_base<epoll_accept> {
    epoll_accept(int fd, struct sockaddr* addr = nullptr,
                 socklen_t* addrlen = nullptr, int flags = 0) noexcept
        : epoll_awaiter_base(fd, EPOLLIN), addr_(addr), addrlen_(addrlen), flags_(flags)
    {
        op_ctx_.perform = &epoll_accept::do_perform;
    }

private:
    struct sockaddr* addr_;
    socklen_t* addrlen_;
    int flags_;

    static int do_perform(int fd, void* self) noexcept {
        auto* a = static_cast<epoll_accept*>(self);
        // 使用 accept4 而非 accept：可以一次指定 SOCK_NONBLOCK 和 SOCK_CLOEXEC，
        // 避免额外调用 fcntl 设置 O_NONBLOCK。减少了一次 syscall。
        int client_fd = ::accept4(fd, a->addr_, a->addrlen_,
                                  SOCK_NONBLOCK | SOCK_CLOEXEC | a->flags_);
        a->io_info_.result = (client_fd >= 0) ? client_fd : -errno;
        return a->io_info_.result;
    }
};

struct epoll_connect final : epoll_awaiter_base<epoll_connect> {
    epoll_connect(int fd, const struct sockaddr* addr, socklen_t addrlen) noexcept
        : epoll_awaiter_base(fd, EPOLLOUT)
    {
        // 对于非阻塞 connect，调用 connect() 后通常会返回 EINPROGRESS。
        // 此时需要等待 EPOLLOUT 事件（表示连接建立完成），然后检查 SO_ERROR。
        // 如果 connect() 直接返回 0，说明连接已经同步建立（罕见，如同机回环地址）。
        // Initiate non-blocking connect
        int ret = ::connect(fd, addr, addrlen);
        if (ret == 0) {
            connect_result_ = 0;
        } else if (errno == EINPROGRESS) {
            connect_result_ = 0; // Will check SO_ERROR in perform
            // connect 正在异步进行，需要等待 EPOLLOUT 事件
        } else {
            connect_result_ = -errno;
            // connect 失败直接返回错误码
        }
        op_ctx_.perform = &epoll_connect::do_perform;
    }

private:
    int32_t connect_result_;

    static int do_perform(int fd, void* self) noexcept {
        auto* a = static_cast<epoll_connect*>(self);
        if (a->connect_result_ < 0) {
            // connect 已经失败，直接返回错误
            a->io_info_.result = a->connect_result_;
            return a->io_info_.result;
        }
        // 通过 getsockopt SO_ERROR 检查异步连接的结果
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0) {
            a->io_info_.result = (err == 0) ? 0 : -err;
        } else {
            a->io_info_.result = -errno;
        }
        return a->io_info_.result;
    }
};

// ============================================================
// Socket control — synchronous (immediate completion)
// ============================================================
// Socket 控制操作 — 同步（立即完成）
// close 和 shutdown 是同步操作，不经过 epoll。
// 它们通过 name-hiding 覆盖 perform_sync_op() 方法。
// CRTP 的 await_suspend 检测到 is_async_ == false 时会直接调用 perform_sync_op()
// 并在完成后通过 forward_task 恢复协程。

struct epoll_close final : epoll_awaiter_base<epoll_close> {
    explicit epoll_close(int fd) noexcept
        : epoll_awaiter_base(fd) {}

    // Name-hiding override (CRTP dispatch from await_suspend)
    // name-hiding 覆盖（从 await_suspend 通过 CRTP 派发）
    int32_t perform_sync_op() noexcept {
        int ret = ::close(fd_);
        return (ret == 0) ? 0 : -errno;
    }
};

struct epoll_shutdown final : epoll_awaiter_base<epoll_shutdown> {
    epoll_shutdown(int fd, int how) noexcept
        : epoll_awaiter_base(fd), how_(how) {}

    int32_t perform_sync_op() noexcept {
        int ret = ::shutdown(fd_, how_);
        return (ret == 0) ? 0 : -errno;
    }

private:
    int how_;
};

// ============================================================
// File I/O — background thread (regular files not epollable)
// ============================================================
// 文件 I/O — 后台线程（普通文件不支持 epoll）
//
// 为什么普通文件不适用于 epoll？
//   epoll 的底层机制是等待设备驱动报告文件描述符的状态变化。
//   对于 socket 和 pipe，内核可以监控接收缓冲区/发送缓冲区的状态变化。
//   但对于普通文件（磁盘文件），内核总是认为它们是"就绪"的，
//   因为文件的 read/write 永远不会阻塞（数据直接从内核页缓存读取）。
//   这意味着 epoll_wait 在普通文件上会立即返回就绪事件，实际上退化为轮询。
//
// 解决方案：pipe + 后台线程
//   - 后台线程执行实际的阻塞式文件 I/O（read/pread/write/pwrite）
//   - I/O 完成后向 pipe 写入一个字节的信号
//   - 前台线程将 pipe 的读端注册到 epoll，等待信号到来
//   - epoll 检测到 pipe 可读时，从 io_info_ 中获取后台线程写入的结果
//
// CRTP 的 register_with_epoll() 覆盖：
//   epoll_read 和 epoll_write 覆盖了 register_with_epoll() 方法。
//   在覆盖的实现中：
//     1. 创建 pipe2（O_NONBLOCK | O_CLOEXEC）
//     2. 将当前 fd_ 替换为 pipe 读端
//     3. 调用基类的 register_with_epoll() 将 pipe 读端注册到 epoll
//     4. 启动后台线程执行文件 I/O
//     5. 后台线程完成后向 pipe 写端写入信号
//     6. 关闭 pipe 写端

struct epoll_read final : epoll_awaiter_base<epoll_read> {
    friend class epoll_awaiter_base<epoll_read>;

    epoll_read(int fd, std::span<char> buf, uint64_t offset = uint64_t(-1)) noexcept
        : epoll_awaiter_base(fd, EPOLLIN), buf_(buf), offset_(offset)
    {
        op_ctx_.perform = &epoll_read::do_perform;
    }

private:
    std::span<char> buf_;
    uint64_t offset_;
    int signal_fd_ = -1;
    int pipe_rd_ = -1;

    // Name-hiding override: create pipe + background thread, then call base
    // name-hiding 覆盖：创建 pipe + 后台线程，然后调用基类的 register_with_epoll
    void register_with_epoll() noexcept {
        int file_fd = fd_;
        int pipefd[2];
        if (::pipe2(pipefd, kPipe2Flags) == 0) {
            pipe_rd_ = pipefd[0];
            signal_fd_ = pipefd[1];
            fd_ = pipe_rd_;
            op_ctx_.fd = fd_;
            epoll_events_ = EPOLLIN;
        }
        // Call the default base implementation to register with epoll
        // 调用基类的默认实现将 pipe 读端注册到 epoll
        epoll_awaiter_base::register_with_epoll();

        auto buf_span = buf_;
        auto off = offset_;
        auto* ti = &io_info_;
        int sig_fd = signal_fd_;

        // 后台线程执行文件 I/O，完成后通过 pipe 发送信号
        std::thread([file_fd, buf_span, off, ti, sig_fd]() {
            ssize_t n;
            if (off == uint64_t(-1)) {
                n = ::read(file_fd, buf_span.data(), buf_span.size());
            } else {
                n = ::pread(file_fd, buf_span.data(), buf_span.size(),
                            static_cast<off_t>(off));
            }
            ti->result = (n >= 0) ? static_cast<int32_t>(n) : -errno;
            uint8_t sig = 1;
            ssize_t nw = ::write(sig_fd, &sig, 1);
            // 向 pipe 写入信号字节，通知前台线程文件 I/O 已完成
            (void)nw;
            ::close(sig_fd);
        }).detach();
    }

    static int do_perform(int fd, void* self) noexcept {
        auto* a = static_cast<epoll_read*>(self);
        uint8_t dummy;
        ssize_t nr = ::read(fd, &dummy, 1);
        // 从 pipe 读取信号字节（实际结果已存储在 io_info_.result 中）
        (void)nr;
        if (a->pipe_rd_ >= 0) {
            ::close(a->pipe_rd_);
            a->pipe_rd_ = -1;
        }
        return a->io_info_.result;
    }
};

struct epoll_write final : epoll_awaiter_base<epoll_write> {
    friend class epoll_awaiter_base<epoll_write>;

    epoll_write(int fd, std::span<const char> buf, uint64_t offset = uint64_t(-1)) noexcept
        : epoll_awaiter_base(fd, EPOLLIN), buf_(buf), offset_(offset)
    {
        op_ctx_.perform = &epoll_write::do_perform;
    }

private:
    std::span<const char> buf_;
    uint64_t offset_;
    int signal_fd_ = -1;
    int pipe_rd_ = -1;

    void register_with_epoll() noexcept {
        int file_fd = fd_;
        int pipefd[2];
        if (::pipe2(pipefd, kPipe2Flags) == 0) {
            pipe_rd_ = pipefd[0];
            signal_fd_ = pipefd[1];
            fd_ = pipe_rd_;
            op_ctx_.fd = fd_;
            epoll_events_ = EPOLLIN;
        }
        epoll_awaiter_base::register_with_epoll();

        auto buf_span = buf_;
        auto off = offset_;
        auto* ti = &io_info_;
        int sig_fd = signal_fd_;

        std::thread([file_fd, buf_span, off, ti, sig_fd]() {
            ssize_t n;
            if (off == uint64_t(-1)) {
                n = ::write(file_fd, buf_span.data(), buf_span.size());
            } else {
                n = ::pwrite(file_fd, buf_span.data(), buf_span.size(),
                             static_cast<off_t>(off));
            }
            ti->result = (n >= 0) ? static_cast<int32_t>(n) : -errno;
            uint8_t sig = 1;
            ssize_t nw = ::write(sig_fd, &sig, 1);
            (void)nw;
            ::close(sig_fd);
        }).detach();
    }

    static int do_perform(int fd, void* self) noexcept {
        auto* a = static_cast<epoll_write*>(self);
        uint8_t dummy;
        ssize_t nr = ::read(fd, &dummy, 1);
        (void)nr;
        if (a->pipe_rd_ >= 0) {
            ::close(a->pipe_rd_);
            a->pipe_rd_ = -1;
        }
        return a->io_info_.result;
    }
};

// ============================================================
// Control / yield / nop — synchronous (immediate completion)
// ============================================================
// 控制操作 / yield / nop — 同步（立即完成）

struct epoll_nop final : epoll_awaiter_base<epoll_nop> {
    epoll_nop() noexcept : epoll_awaiter_base(-1) {}
    int32_t perform_sync_op() noexcept { return 0; }
};

struct epoll_yield final : epoll_awaiter_base<epoll_yield> {
    epoll_yield() noexcept : epoll_awaiter_base(-1) {}
    int32_t perform_sync_op() noexcept { return 0; }
};

// ============================================================
// Timeout — timerfd (async, epoll-based)
// ============================================================
// 超时 — timerfd（异步，基于 epoll）
//
// timerfd 是 Linux 提供的定时器文件描述符机制：
//   - timerfd_create() 创建一个定时器 fd
//   - timerfd_settime() 设置定时器的超时时间
//   - 定时器到期时，timerfd 变为可读（EPOLLIN）
//   - 可以通过 epoll 监听 timerfd 的 EPOLLIN 事件来实现异步超时
//
// 与 io_uring 的 timeout 操作不同：
//   io_uring 可以直接提交 IORING_OP_TIMEOUT 类型的 SQE，
//   内核在超时后自动产生 CQE，不需要单独的 fd。
//   epoll 需要借助 timerfd 将定时器转换为 fd 事件。
//
// 为什么使用 timerfd 而非其他方案？
//   - sleep + 信号：需要处理信号中断，复杂且不优雅
//   - eventfd + 后台线程：需要额外的线程开销
//   - timerfd：直接将定时器暴露为 fd，可以无缝集成到 epoll 事件循环中

struct epoll_timeout final : epoll_awaiter_base<epoll_timeout> {
    template<typename Rep, typename Period>
    explicit epoll_timeout(const std::chrono::duration<Rep, Period>& dur) noexcept
        : epoll_awaiter_base(-1, EPOLLIN)
    {
        timerfd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (timerfd_ >= 0) {
            fd_ = timerfd_;
            op_ctx_.fd = fd_;
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
            if (ns < 0) ns = 0;
            struct itimerspec its{};
            its.it_value.tv_sec  = static_cast<time_t>(ns / 1'000'000'000LL);
            its.it_value.tv_nsec = static_cast<long>(ns % 1'000'000'000LL);
            ::timerfd_settime(timerfd_, 0, &its, nullptr);
            // timerfd_settime 的第二个参数 flags=0 表示相对时间
        }
        op_ctx_.perform = &epoll_timeout::do_perform;
    }

private:
    int timerfd_ = -1;

    static int do_perform(int fd, void* self) noexcept {
        auto* a = static_cast<epoll_timeout*>(self);
        uint64_t expirations = 0;
        // 读取 timerfd 的到期次数（必须读取才能清除 EPOLLIN 状态）
        ssize_t nr = ::read(fd, &expirations, sizeof(expirations));
        (void)nr;
        if (a->timerfd_ >= 0) {
            ::close(a->timerfd_);
            a->timerfd_ = -1;
        }
        a->io_info_.result = 0;
        return 0;
    }
};

} // namespace coronet::detail

// ============================================================
// Platform factory functions — uniform interface for async_io.hpp
// ============================================================
// 平台工厂函数 — 为 async_io.hpp 提供统一接口
// 与 io_uring 和 IOCP 版本保持一致的命名空间和函数签名
// async_io.hpp 通过 #ifdef 在编译期选择具体的 platform_io 实现

namespace coronet::detail::platform_io {

inline auto make_recv(int fd, std::span<char> buf, int flags) noexcept
    { return epoll_recv{fd, buf, flags}; }

inline auto make_send(int fd, std::span<const char> buf, int flags) noexcept
    { return epoll_send{fd, buf, flags}; }

inline auto make_accept(int fd, struct sockaddr* a, socklen_t* al, int fl) noexcept
    { return epoll_accept{fd, a, al, fl}; }

inline auto make_connect(int fd, const struct sockaddr* a, socklen_t al) noexcept
    { return epoll_connect{fd, a, al}; }

inline auto make_close(int fd) noexcept
    { return epoll_close{fd}; }

inline auto make_shutdown(int fd, int how) noexcept
    { return epoll_shutdown{fd, how}; }

inline auto make_read(int fd, std::span<char> buf, uint64_t off) noexcept
    { return epoll_read{fd, buf, off}; }

inline auto make_write(int fd, std::span<const char> buf, uint64_t off) noexcept
    { return epoll_write{fd, buf, off}; }

inline auto make_yield() noexcept
    { return epoll_yield{}; }

template<typename D>
inline auto make_timeout(D dur) noexcept
    { return epoll_timeout{dur}; }

} // namespace coronet::detail::platform_io
