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
namespace {
constexpr int kPipe2Flags = 04000 | 02000000;
}

namespace coronet::detail {

// ============================================================
// epoll_awaiter_base<Derived> — CRTP base for epoll awaitables
// ============================================================
// Replaces virtual dispatch with compile-time CRTP:
//   - await_suspend / do_issue_io dispatch to Derived::register_with_epoll()
//     and Derived::perform_sync_op() with zero runtime overhead (static_cast).
//   - Default implementations for register_with_epoll() and perform_sync_op()
//     live in the base; derived types override by name-hiding.
//   - The perform callback (op_ctx_.perform) is already a static function
//     pointer — no change needed there.

template<typename Derived>
class epoll_awaiter_base {
public:
    [[nodiscard]] int32_t result() const noexcept { return io_info_.result; }
    static constexpr bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> current) noexcept {
        io_info_.handle = current;
        if (is_async_) {
            static_cast<Derived*>(this)->register_with_epoll();
        } else {
            // Synchronous: complete immediately without epoll
            io_info_.result = static_cast<Derived*>(this)->perform_sync_op();
            // requests_to_reap was NOT incremented for sync ops
            this_thread.worker->forward_task(current);
            io_info_.handle = nullptr;
        }
    }

    [[nodiscard]] int32_t await_resume() const noexcept { return result(); }

    [[nodiscard]] uint64_t user_data() const noexcept {
        return io_info_.as_user_data();
    }

    // Public for chained co_await — dispatches via CRTP
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
    epoll_awaiter_base(int fd, uint32_t epoll_events) noexcept
        : fd_(fd), epoll_events_(epoll_events), is_async_(true) {
        op_ctx_.self = static_cast<Derived*>(this);
        op_ctx_.fd = fd;
        op_ctx_.user_data = io_info_.as_user_data()
            | uint64_t(user_data_type::task_info_ptr);
        ++this_thread.worker->requests_to_reap;
    }

    /// Constructor for sync ops (complete immediately, no epoll).
    explicit epoll_awaiter_base(int fd) noexcept
        : fd_(fd), epoll_events_(0), is_async_(false) {
        op_ctx_.fd = fd;
    }

    // ---- Default implementations (overridden by name-hiding in derived) ----

    /// Default epoll registration. Override in async derived types that need
    /// custom setup (e.g. epoll_read / epoll_write create a pipe first).
    void register_with_epoll() noexcept {
        auto* p = static_cast<platform::epoll::epoll_proactor*>(
            this_thread.worker->proactor);
        p->register_fd(fd_, epoll_events_, &op_ctx_);
    }

    /// Default sync operation (no-op). Override in sync derived types.
    int32_t perform_sync_op() noexcept { return 0; }

public:
    // Public: accessed by chained_awaiter (operator&&)
    task_info io_info_;

    // Completion context — stored in epoll_event.data.ptr
    platform::epoll::epoll_completion_ctx op_ctx_{};

protected:
    int fd_;
    uint32_t epoll_events_;
    bool is_async_;
};

// ============================================================
// Helper: perform a non-blocking I/O syscall, return result
// ============================================================
namespace {

inline int32_t sys_result(ssize_t n) noexcept {
    return (n >= 0) ? static_cast<int32_t>(n) : -errno;
}

} // anonymous namespace

// ============================================================
// Socket I/O — async (epoll-based, default register_with_epoll)
// ============================================================

struct epoll_recv final : epoll_awaiter_base<epoll_recv> {
    epoll_recv(int fd, std::span<char> buf, int flags = 0) noexcept
        : epoll_awaiter_base(fd, EPOLLIN), buf_(buf), flags_(flags)
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
        // Initiate non-blocking connect
        int ret = ::connect(fd, addr, addrlen);
        if (ret == 0) {
            connect_result_ = 0;
        } else if (errno == EINPROGRESS) {
            connect_result_ = 0; // Will check SO_ERROR in perform
        } else {
            connect_result_ = -errno;
        }
        op_ctx_.perform = &epoll_connect::do_perform;
    }

private:
    int32_t connect_result_;

    static int do_perform(int fd, void* self) noexcept {
        auto* a = static_cast<epoll_connect*>(self);
        if (a->connect_result_ < 0) {
            a->io_info_.result = a->connect_result_;
            return a->io_info_.result;
        }
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

struct epoll_close final : epoll_awaiter_base<epoll_close> {
    explicit epoll_close(int fd) noexcept
        : epoll_awaiter_base(fd) {}

    // Name-hiding override (CRTP dispatch from await_suspend)
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
        epoll_awaiter_base::register_with_epoll();

        auto buf_span = buf_;
        auto off = offset_;
        auto* ti = &io_info_;
        int sig_fd = signal_fd_;

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
            (void)nw;
            ::close(sig_fd);
        }).detach();
    }

    static int do_perform(int fd, void* self) noexcept {
        auto* a = static_cast<epoll_read*>(self);
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
        }
        op_ctx_.perform = &epoll_timeout::do_perform;
    }

private:
    int timerfd_ = -1;

    static int do_perform(int fd, void* self) noexcept {
        auto* a = static_cast<epoll_timeout*>(self);
        uint64_t expirations = 0;
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
