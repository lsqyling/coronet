#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include "coronet/detail/task_info.hpp"
#include "coronet/detail/thread_meta.hpp"
#include "coronet/detail/worker_meta.hpp"
#include "coronet/detail/user_data.hpp"
#include "coronet/platform/iocp/iocp_proactor.hpp"

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <span>
#include <winsock2.h>
#include <mswsock.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>        // _read, _write

namespace coronet::detail {

// ============================================================
// Base awaiter for IOCP I/O
// ============================================================

class win_awaiter {
public:
    [[nodiscard]] int32_t result() const noexcept { return io_info_.result; }
    static constexpr bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> current) noexcept {
        io_info_.handle = current;
        io_info_.result = 0;
        auto* p = static_cast<platform::iocp::iocp_proactor*>(
            this_thread.worker->proactor);
        p->work_started();
        issue_io();
    }

    [[nodiscard]] int32_t await_resume() const noexcept { return result(); }
    [[nodiscard]] uint64_t user_data() const noexcept {
        return io_info_.as_user_data();
    }

    template<typename, typename> friend struct chained_awaiter;

    // Public for chained co_await
    void do_issue_io() noexcept { issue_io(); }
    void refresh_user_data() noexcept { op_->set_user_data(io_info_.as_user_data()); }

protected:
    win_awaiter() noexcept {
        auto* p = static_cast<platform::iocp::iocp_proactor*>(
            this_thread.worker->proactor);
        op_ = p->acquire_operation();
        if (op_) op_->set_user_data(io_info_.as_user_data());
    }

    virtual void issue_io() noexcept = 0;

    void finish_issue(DWORD ioresult, DWORD /*bytes*/) noexcept {
        auto* p = static_cast<platform::iocp::iocp_proactor*>(
            this_thread.worker->proactor);
        if (ioresult == 0) {
            op_->on_pending(p);
        } else {
            DWORD err = ::WSAGetLastError();
            if (err == WSA_IO_PENDING) {
                op_->on_pending(p);
            } else {
                io_info_.result = -static_cast<int32_t>(err);
                p->post_completion(op_.get(), 0, 0);
            }
        }
        op_.release();
    }

public:
    std::unique_ptr<platform::iocp::iocp_operation> op_;
    // Public: accessed by chained_awaiter, handle_completion, operator&&
    task_info io_info_;
    uintptr_t sock_ = 0;
};

// ============================================================
// AcceptEx / ConnectEx dynamic loading
// ============================================================

namespace {

inline LPFN_ACCEPTEX load_accept_ex() noexcept {
    GUID guid = WSAID_ACCEPTEX;
    LPFN_ACCEPTEX ptr = nullptr;
    DWORD bytes = 0;
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s != INVALID_SOCKET) {
        WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &guid, sizeof(guid), &ptr, sizeof(ptr),
                 &bytes, nullptr, nullptr);
        ::closesocket(s);
    }
    return ptr;
}

inline LPFN_CONNECTEX load_connect_ex() noexcept {
    GUID guid = WSAID_CONNECTEX;
    LPFN_CONNECTEX ptr = nullptr;
    DWORD bytes = 0;
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s != INVALID_SOCKET) {
        WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &guid, sizeof(guid), &ptr, sizeof(ptr),
                 &bytes, nullptr, nullptr);
        ::closesocket(s);
    }
    return ptr;
}

inline LPFN_ACCEPTEX get_accept_ex() noexcept {
    static LPFN_ACCEPTEX fn = load_accept_ex();
    return fn;
}

inline LPFN_CONNECTEX get_connect_ex() noexcept {
    static LPFN_CONNECTEX fn = load_connect_ex();
    return fn;
}

} // anonymous namespace

// ============================================================
// Concrete I/O operations
// ============================================================

struct win_recv final : win_awaiter {
    win_recv(uintptr_t sock, std::span<char> buf, int flags = 0) noexcept
        : win_awaiter() { sock_ = sock; buf_ = buf; flags_ = flags; }

private:
    void issue_io() noexcept override {
        WSABUF wbuf{.len = static_cast<ULONG>(buf_.size()), .buf = buf_.data()};
        DWORD fl = flags_, bytes = 0;
        int ret = ::WSARecv((SOCKET)sock_, &wbuf, 1, &bytes, &fl,
            static_cast<OVERLAPPED*>(op_->native_overlapped()), nullptr);
        finish_issue(ret, bytes);
    }
    std::span<char> buf_;
    int flags_ = 0;
};

struct win_send final : win_awaiter {
    win_send(uintptr_t sock, std::span<const char> buf, int flags = 0) noexcept
        : win_awaiter() {
        sock_ = sock;
        wbuf_ = WSABUF{.len = static_cast<ULONG>(buf.size()),
                        .buf = const_cast<char*>(buf.data())};
        flags_ = flags;
    }

private:
    void issue_io() noexcept override {
        DWORD bytes = 0;
        int ret = ::WSASend((SOCKET)sock_, &wbuf_, 1, &bytes, flags_,
            static_cast<OVERLAPPED*>(op_->native_overlapped()), nullptr);
        finish_issue(ret, bytes);
    }
    WSABUF wbuf_{};
    int flags_ = 0;
};

struct win_accept final : win_awaiter {
    win_accept(uintptr_t sock, struct sockaddr* addr = nullptr,
               socklen_t* addrlen = nullptr, int flags = 0) noexcept
        : win_awaiter() {
        sock_ = sock; addr_ = addr; addrlen_ = addrlen; (void)flags;
        create_accept_socket();
    }

    [[nodiscard]] int32_t await_resume() const noexcept {
        return static_cast<int32_t>(accept_socket_);
    }

private:
    void create_accept_socket() noexcept {
        accept_socket_ = ::WSASocketW(AF_INET, SOCK_STREAM, 0,
                                       nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (accept_socket_ != INVALID_SOCKET) {
            auto* p = static_cast<platform::iocp::iocp_proactor*>(
                this_thread.worker->proactor);
            p->register_handle(accept_socket_);
        }
    }

    void issue_io() noexcept override {
        LPFN_ACCEPTEX fn = get_accept_ex();
        if (!fn || accept_socket_ == INVALID_SOCKET) {
            accept_socket_ = INVALID_SOCKET;
            io_info_.result = -1;
            finish_issue(1, 0);
            return;
        }

        auto* p = static_cast<platform::iocp::iocp_proactor*>(
            this_thread.worker->proactor);
        p->register_handle(sock_);

        DWORD addr_buf_len = sizeof(sockaddr_storage) + 16;
        memset(addr_buf_, 0, sizeof(addr_buf_));
        DWORD bytes_received = 0;
        BOOL ok = fn((SOCKET)sock_, accept_socket_, addr_buf_,
            0, addr_buf_len, addr_buf_len, &bytes_received,
            static_cast<OVERLAPPED*>(op_->native_overlapped()));

        if (!ok) {
            DWORD err = ::WSAGetLastError();
            if (err == WSA_IO_PENDING) {
                op_->on_pending(p);
                op_.release();
            } else {
                ::closesocket(accept_socket_);
                if (err == ERROR_CONNECTION_ABORTED) {
                    create_accept_socket();
                    if (accept_socket_ != INVALID_SOCKET) {
                        issue_io();
                        return;
                    }
                }
                accept_socket_ = INVALID_SOCKET;
                io_info_.result = -static_cast<int32_t>(err);
                finish_issue(1, 0);
                op_.release();
            }
        } else {
            op_->Internal = 0;
            op_->InternalHigh = bytes_received;
            finish_issue(0, bytes_received);
            op_.release();
        }
    }

    struct sockaddr* addr_ = nullptr;
    socklen_t* addrlen_ = nullptr;
    uintptr_t accept_socket_ = coronet::platform::invalid_socket;
    char addr_buf_[sizeof(sockaddr_storage) * 2 + 32]{};
};

struct win_connect final : win_awaiter {
    win_connect(uintptr_t sock, const struct sockaddr* addr,
                socklen_t addrlen) noexcept
        : win_awaiter() { sock_ = sock; addr_ = addr; addrlen_ = addrlen; }

private:
    void issue_io() noexcept override {
        LPFN_CONNECTEX fn = get_connect_ex();
        if (!fn) { io_info_.result = -1; finish_issue(1, 0); return; }

        auto* p = static_cast<platform::iocp::iocp_proactor*>(
            this_thread.worker->proactor);
        p->register_handle(sock_);

        struct sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port = 0;
        ::bind((SOCKET)sock_, reinterpret_cast<struct sockaddr*>(&local),
               sizeof(local));

        BOOL ok = fn((SOCKET)sock_, addr_, addrlen_, nullptr, 0, nullptr,
            static_cast<OVERLAPPED*>(op_->native_overlapped()));

        if (!ok) {
            DWORD err = ::WSAGetLastError();
            if (err == WSA_IO_PENDING) {
                op_->on_pending(p);
                op_.release();
            } else {
                io_info_.result = -static_cast<int32_t>(err);
                finish_issue(1, 0);
            }
        } else {
            finish_issue(0, 0);
        }
    }
    const struct sockaddr* addr_ = nullptr;
    socklen_t addrlen_ = 0;
};

struct win_close final : win_awaiter {
    explicit win_close(uintptr_t sock) noexcept : win_awaiter() { sock_ = sock; }
private:
    void issue_io() noexcept override {
        ::closesocket((SOCKET)sock_);
        io_info_.result = 0;
        // Post completion manually — closesocket is synchronous, not overlapped
        op_->on_sync_completion(
            static_cast<platform::iocp::iocp_proactor*>(
                this_thread.worker->proactor), 0);
        (void)op_.release();
    }
};

struct win_nop final : win_awaiter {
    win_nop() noexcept : win_awaiter() {}
private:
    void issue_io() noexcept override {
        io_info_.result = 0;
        // Post completion manually — nop is synchronous, not overlapped
        op_->on_sync_completion(
            static_cast<platform::iocp::iocp_proactor*>(
                this_thread.worker->proactor), 0);
        (void)op_.release();
    }
};

/// Windows timeout: delegates blocking wait to a background thread that
/// posts IOCP completion after the delay. The operation is kept alive by
/// the thread via raw pointer ownership transfer.
struct win_timeout final : win_awaiter {
    template<typename Rep, typename Period>
    explicit win_timeout(const std::chrono::duration<Rep, Period>& dur) noexcept
        : win_awaiter() {
        dur_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
        if (dur_ms_ < 1) dur_ms_ = 1;
    }

private:
    void issue_io() noexcept override {
        auto* raw_op = op_.release();        // transfer ownership to background thread
        auto* proactor = static_cast<platform::iocp::iocp_proactor*>(
            this_thread.worker->proactor);
        DWORD ms = static_cast<DWORD>(dur_ms_);
        // Background thread: sleep → signal IOCP completion (via on_sync_completion)
        std::thread([raw_op, proactor, ms]() noexcept {
            Sleep(ms);
            raw_op->on_sync_completion(proactor, 0);
        }).detach();
    }

    long long dur_ms_ = 0;
};

/// Windows async read (file/pipe/console): uses background thread + _read + IOCP.
struct win_read final : win_awaiter {
    win_read(int fd, std::span<char> buf, uint64_t /*offset*/) noexcept
        : win_awaiter() { fd_ = fd; buf_ = buf; }
private:
    void issue_io() noexcept override {
        auto* raw_op = op_.release();
        auto* proactor = static_cast<platform::iocp::iocp_proactor*>(
            this_thread.worker->proactor);
        int f = fd_; auto sp = buf_;
        std::thread([raw_op, proactor, f, sp]() noexcept {
            int n = ::_read(f, sp.data(), static_cast<unsigned>(sp.size()));
            raw_op->on_sync_completion(proactor, (n >= 0) ? static_cast<DWORD>(n) : 0);
        }).detach();
    }
    int fd_ = 0;
    std::span<char> buf_;
};

/// Windows async write (file/pipe/console): uses background thread + _write + IOCP.
struct win_write final : win_awaiter {
    win_write(int fd, std::span<const char> buf, uint64_t /*offset*/) noexcept
        : win_awaiter() { fd_ = fd; buf_ = buf; }
private:
    void issue_io() noexcept override {
        auto* raw_op = op_.release();
        auto* proactor = static_cast<platform::iocp::iocp_proactor*>(
            this_thread.worker->proactor);
        int f = fd_; auto sp = buf_;
        std::thread([raw_op, proactor, f, sp]() noexcept {
            int n = ::_write(f, sp.data(), static_cast<unsigned>(sp.size()));
            raw_op->on_sync_completion(proactor, (n >= 0) ? static_cast<DWORD>(n) : 0);
        }).detach();
    }
    int fd_ = 0;
    std::span<const char> buf_;
};

struct win_shutdown final : win_awaiter {
    win_shutdown(uintptr_t sock, int how) noexcept : win_awaiter() {
        sock_ = sock; how_ = how;
    }
private:
    void issue_io() noexcept override {
        int ret = ::shutdown((SOCKET)sock_, how_);
        io_info_.result = (ret == 0) ? 0 : -::WSAGetLastError();
        // Post completion manually — shutdown is synchronous, not overlapped
        op_->on_sync_completion(
            static_cast<platform::iocp::iocp_proactor*>(
                this_thread.worker->proactor), 0);
        (void)op_.release();
    }
    int how_ = 0;
};

} // namespace coronet::detail

// Platform factory functions — uniform interface for async_io.hpp
namespace coronet::detail::platform_io {
    inline auto make_recv(int fd, std::span<char> buf, int flags) noexcept
        { return win_recv{(uintptr_t)fd, buf, flags}; }
    inline auto make_send(int fd, std::span<const char> buf, int flags) noexcept
        { return win_send{(uintptr_t)fd, buf, flags}; }
    inline auto make_accept(int fd, struct sockaddr* a, socklen_t* al, int fl) noexcept
        { return win_accept{(uintptr_t)fd, a, al, fl}; }
    inline auto make_connect(int fd, const struct sockaddr* a, socklen_t al) noexcept
        { return win_connect{(uintptr_t)fd, a, al}; }
    inline auto make_close(int fd) noexcept
        { return win_close{(uintptr_t)fd}; }
    inline auto make_shutdown(int fd, int how) noexcept
        { return win_shutdown{(uintptr_t)fd, how}; }
    inline auto make_read(int fd, std::span<char> buf, uint64_t off) noexcept
        { return win_read{fd, buf, off}; }
    inline auto make_write(int fd, std::span<const char> buf, uint64_t off) noexcept
        { return win_write{fd, buf, off}; }
    inline auto make_yield() noexcept
        { return win_nop{}; }
    template<typename D>
    inline auto make_timeout(D dur) noexcept
        { return win_timeout{dur}; }
} // namespace coronet::detail::platform_io
