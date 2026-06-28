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
class io_uring_awaiter {
public:
    [[nodiscard]] int32_t result() const noexcept { return io_info_.result; }
    static constexpr bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> current) noexcept {
        io_info_.handle = current;
    }

    [[nodiscard]] int32_t await_resume() const noexcept { return result(); }
    [[nodiscard]] uint64_t user_data() const noexcept {
        return sqe_->get_data();
    }

    // Public for chained co_await
    void do_issue_io() noexcept {}  // io_uring: SQE already prepared
    void refresh_user_data() noexcept {
        sqe_->set_data(io_info_.as_user_data() | uint64_t(user_data_type::task_info_ptr));
    }

protected:
    io_uring_awaiter() noexcept {
        auto* p = static_cast<platform::io_uring::io_uring_proactor*>(
            this_thread.worker->proactor);
        sqe_ = p->get_sq_entry();
        sqe_->set_data(
            io_info_.as_user_data()
            | uint64_t(detail::user_data_type::task_info_ptr));
        // Track the inflight operation count
        ++this_thread.worker->requests_to_reap;
        ++this_thread.worker->requests_to_submit;
    }

public:
    liburingcxx::sq_entry* sqe_ = nullptr;
    // Public: accessed by chained_awaiter (operator&&)
    task_info io_info_;
};

// ============================================================
// Socket I/O
// ============================================================

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

struct io_uring_read : io_uring_awaiter {
    io_uring_read(int fd, std::span<char> buf, uint64_t offset = uint64_t(-1)) noexcept
        : io_uring_awaiter() { sqe_->prep_read(fd, buf, offset); }
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

struct io_uring_nop : io_uring_awaiter {
    io_uring_nop() noexcept : io_uring_awaiter() { sqe_->prep_nop(); }
};

struct io_uring_yield : io_uring_awaiter {
    io_uring_yield() noexcept : io_uring_awaiter() { sqe_->prep_nop(); }
};

struct io_uring_timeout : io_uring_awaiter {
  private:
    __kernel_timespec ts_{};  // MUST be member: SQE stores pointer to this

  public:
    template<typename Rep, typename Period>
    explicit io_uring_timeout(const std::chrono::duration<Rep, Period>& dur) noexcept
        : io_uring_awaiter() {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
        ts_.tv_sec  = static_cast<decltype(ts_.tv_sec)>(ns / 1'000'000'000LL);
        ts_.tv_nsec = static_cast<decltype(ts_.tv_nsec)>(ns % 1'000'000'000LL);
        // co_context alignment: timeout_relative_flag | pure_timer_flag
        sqe_->prep_timeout(ts_, 0, config::timeout_flags);
    }
};

} // namespace coronet::detail

// Platform factory functions — uniform interface for async_io.hpp
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
