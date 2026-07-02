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
// Base awaiter for IOCP I/O — CRTP 编译期多态
// ============================================================
// IOCP awaiter 基类（CRTP 版本）
//
// 与 io_uring 不同，Windows IOCP 的 I/O 提交和完成是分开的：
//   - 提交：在 await_suspend 中通过 CRTP 调用派生类的 issue_io() 发起 Windows I/O API
//   - 完成：内核自动将完成事件投递到 IOCP，wait_completion 从中取出
//
// CRTP vs 虚函数：
//   旧版使用虚函数 issue_io() 让派生类实现具体的 I/O 调用方式。
//   新版使用 CRTP（奇异递归模板模式），在编译期将调用分派到派生类型，
//   消除了 vtable 间接调用开销，使 issue_io() 可以被编译器内联。
//   参考 epoll_lazy_io.hpp 中 epoll_awaiter_base<Derived> 的设计。
//
// 关键设计：
//   1. 通过 finish_issue() 统一处理 I/O 提交结果：根据返回值区分同步完成和异步等待。
//   2. 每个派生类在析构/完成时通过 recycle_operation() 回收 iocp_operation，
//      避免每次 I/O 的堆分配开销。
//   3. work_started()/work_finished() 跟踪飞行中操作，确保事件循环不会在所有操作完成前退出。
//
// I/O 提交协议（finish_issue 中的判断逻辑）：
//   - ioresult == 0：同步完成，WSAGetLastError() == NO_ERROR，调用 on_pending
//   - ioresult != 0 && WSAGetLastError() == WSA_IO_PENDING：异步等待，调用 on_pending
//   - ioresult != 0 && WSAGetLastError() != WSA_IO_PENDING：同步失败，直接 post 错误结果

template<typename Derived>
class win_awaiter_base {
public:
    [[nodiscard]] int32_t result() const noexcept { return io_info_.result; }
    static constexpr bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> current) noexcept {
        io_info_.handle = current;
        io_info_.result = 0;
        auto* p = static_cast<platform::iocp::iocp_proactor*>(
            this_thread.worker->proactor);
        p->work_started();
        // 通过 CRTP 调用派生类的 issue_io() 执行实际的 I/O 操作
        // 编译器可内联此调用，因为派生类型在编译期已知
        static_cast<Derived*>(this)->issue_io();
    }

    [[nodiscard]] int32_t await_resume() const noexcept { return result(); }
    [[nodiscard]] uint64_t user_data() const noexcept {
        return io_info_.as_user_data();
    }

    template<typename, typename> friend struct chained_awaiter;

    // Public for chained co_await
    // 公开给 chained_awaiter（operator&&）使用
    void do_issue_io() noexcept { static_cast<Derived*>(this)->issue_io(); }
    void refresh_user_data() noexcept { op_->set_user_data(io_info_.as_user_data()); }

protected:
    win_awaiter_base() noexcept {
        // 从 proactor 获取（或回收复用）一个 iocp_operation
        auto* p = static_cast<platform::iocp::iocp_proactor*>(
            this_thread.worker->proactor);
        op_ = p->acquire_operation();
        if (op_) op_->set_user_data(io_info_.as_user_data());
    }

    // 注意：不再有纯虚函数 issue_io()。
    // 派生类提供 issue_io() 方法，通过 CRTP 在编译期分派。
    // 如果派生类忘记实现，在链接时会得到"未定义引用"错误。

    void finish_issue(DWORD ioresult, DWORD /*bytes*/) noexcept {
        // 完成 I/O 提交的统一处理：
        //   1. 如果 ioresult == 0（同步成功），调用 on_pending 但操作已经完成
        //   2. 如果 WSAGetLastError() == WSA_IO_PENDING，真正的异步等待
        //   3. 其他错误，直接 post 失败结果给协程
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
        // 释放 ownership：operation 的所有权转移到 IOCP 或完成回调中
    }

public:
    std::unique_ptr<platform::iocp::iocp_operation> op_;
    // Public: accessed by chained_awaiter, handle_completion, operator&&
    // 公开成员：供 chained_awaiter、handle_completion 和 operator&& 访问
    task_info io_info_;
    uintptr_t sock_ = 0;
};

// ============================================================
// AcceptEx / ConnectEx dynamic loading
// ============================================================
// AcceptEx / ConnectEx 动态加载
//
// 为什么需要动态加载？
//   AcceptEx 和 ConnectEx 是 Microsoft 在 Winsock2 之后增加的扩展 API，
//   不在标准 winsock2.h 中导出，需要通过 WSAIoctl + SIO_GET_EXTENSION_FUNCTION_POINTER
//   动态获取函数指针。这是 Windows 套接字扩展的通用模式。
//
//   AcceptEx 的特点：
//     - 支持接受连接的同时读取第一个数据包（减少一次 context switch）
//     - 需要在调用前预先创建 accept socket
//     - 需要提供两个地址缓冲区（本地地址 + 远程地址），大小至少必须为 sizeof(sockaddr_storage) + 16
//     - 连接接受后需要调用 setsockopt(SO_UPDATE_ACCEPT_CONTEXT) 使新 socket 继承监听 socket 的属性
//
//   ConnectEx 的特点：
//     - 支持连接的异步发起
//     - 调用前 socket 必须已经 bind 到本地地址
//     - 调用前 socket 必须是未连接的

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
    // 静态局部变量：首次调用时加载，后续复用
    static LPFN_ACCEPTEX fn = load_accept_ex();
    return fn;
}

inline LPFN_CONNECTEX get_connect_ex() noexcept {
    static LPFN_CONNECTEX fn = load_connect_ex();
    return fn;
}

} // anonymous namespace

// ============================================================
// Concrete I/O operations — CRTP 派生类型
// ============================================================
// 具体的 I/O 操作实现
// 每个类型继承 win_awaiter_base<自身>，实现 issue_io() 方法。
// 不再使用 override 关键字（CRTP 编译期分派，非虚函数覆盖）。

struct win_recv final : win_awaiter_base<win_recv> {
    win_recv(uintptr_t sock, std::span<char> buf, int flags = 0) noexcept
        : win_awaiter_base() { sock_ = sock; buf_ = buf; flags_ = flags; }

private:
    void issue_io() noexcept {
        // WSARecv 是 Windows 的异步 socket 接收 API。
        // WSABUF 是 Windows 的 scatter/gather I/O 缓冲区描述符。
        WSABUF wbuf{.len = static_cast<ULONG>(buf_.size()), .buf = buf_.data()};
        DWORD fl = flags_, bytes = 0;
        int ret = ::WSARecv((SOCKET)sock_, &wbuf, 1, &bytes, &fl,
            static_cast<OVERLAPPED*>(op_->native_overlapped()), nullptr);
        finish_issue(ret, bytes);
    }
    std::span<char> buf_;
    int flags_ = 0;
};

struct win_send final : win_awaiter_base<win_send> {
    win_send(uintptr_t sock, std::span<const char> buf, int flags = 0) noexcept
        : win_awaiter_base() {
        sock_ = sock;
        wbuf_ = WSABUF{.len = static_cast<ULONG>(buf.size()),
                        .buf = const_cast<char*>(buf.data())};
        flags_ = flags;
    }

private:
    void issue_io() noexcept {
        // WSASend 是 Windows 的异步 socket 发送 API。
        DWORD bytes = 0;
        int ret = ::WSASend((SOCKET)sock_, &wbuf_, 1, &bytes, flags_,
            static_cast<OVERLAPPED*>(op_->native_overlapped()), nullptr);
        finish_issue(ret, bytes);
    }
    WSABUF wbuf_{};
    int flags_ = 0;
};

struct win_accept final : win_awaiter_base<win_accept> {
    win_accept(uintptr_t sock, struct sockaddr* addr = nullptr,
               socklen_t* addrlen = nullptr, int flags = 0) noexcept
        : win_awaiter_base() {
        sock_ = sock; addr_ = addr; addrlen_ = addrlen; (void)flags;
        create_accept_socket();
    }

    [[nodiscard]] int32_t await_resume() const noexcept {
        // await_resume 返回 accept 的新 socket 句柄（而非字节数）
        // 这是 IOCP 特有的：AcceptEx 需要预先创建 accept socket
        return static_cast<int32_t>(accept_socket_);
    }

private:
    void create_accept_socket() noexcept {
        // AcceptEx 要求预先创建一个 socket 用于接受连接。
        // 使用 WSA_FLAG_OVERLAPPED 标志使其支持重叠 I/O。
        accept_socket_ = ::WSASocketW(AF_INET, SOCK_STREAM, 0,
                                       nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (accept_socket_ != INVALID_SOCKET) {
            auto* p = static_cast<platform::iocp::iocp_proactor*>(
                this_thread.worker->proactor);
            // 新 socket 必须关联到 IOCP，否则完成事件无法投递
            p->register_handle(accept_socket_);
        }
    }

    void issue_io() noexcept {
        LPFN_ACCEPTEX fn = get_accept_ex();
        if (!fn || accept_socket_ == INVALID_SOCKET) {
            accept_socket_ = INVALID_SOCKET;
            io_info_.result = -1;
            finish_issue(1, 0);
            return;
        }

        auto* p = static_cast<platform::iocp::iocp_proactor*>(
            this_thread.worker->proactor);
        // 监听 socket 也需要关联到 IOCP
        p->register_handle(sock_);

        // AcceptEx 地址缓冲区：
        //   需要同时容纳本地地址和远程地址，每个地址需要额外的 16 字节填充。
        //   sizeof(sockaddr_storage) * 2 + 32 确保足够大。
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
                    // 连接被中止：典型的 Windows accept 错误，需要重试
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
            // 同步完成（罕见但可能）
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

struct win_connect final : win_awaiter_base<win_connect> {
    win_connect(uintptr_t sock, const struct sockaddr* addr,
                socklen_t addrlen) noexcept
        : win_awaiter_base() { sock_ = sock; addr_ = addr; addrlen_ = addrlen; }

private:
    void issue_io() noexcept {
        LPFN_CONNECTEX fn = get_connect_ex();
        if (!fn) { io_info_.result = -1; finish_issue(1, 0); return; }

        auto* p = static_cast<platform::iocp::iocp_proactor*>(
            this_thread.worker->proactor);
        p->register_handle(sock_);

        // ConnectEx 要求 socket 必须先 bind
        // 绑定到 INADDR_ANY + 端口 0 让系统自动选择
        struct sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port = 0;
        ::bind((SOCKET)sock_, reinterpret_cast<struct sockaddr*>(&local),
               sizeof(local));

        // 调用 ConnectEx 异步发起连接
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
            // 同步连接成功（罕见）
            finish_issue(0, 0);
        }
    }
    const struct sockaddr* addr_ = nullptr;
    socklen_t addrlen_ = 0;
};

struct win_close final : win_awaiter_base<win_close> {
    explicit win_close(uintptr_t sock) noexcept : win_awaiter_base() { sock_ = sock; }
private:
    void issue_io() noexcept {
        // closesocket 是同步操作，没有重叠 I/O 版本。
        // 执行完后通过 on_sync_completion 手动 post 完成事件到 IOCP。
        ::closesocket((SOCKET)sock_);
        io_info_.result = 0;
        // Post completion manually — closesocket is synchronous, not overlapped
        // 手动 post 完成事件 — closesocket 是同步的，不是重叠 I/O
        op_->on_sync_completion(
            static_cast<platform::iocp::iocp_proactor*>(
                this_thread.worker->proactor), 0);
        (void)op_.release();
    }
};

struct win_nop final : win_awaiter_base<win_nop> {
    win_nop() noexcept : win_awaiter_base() {}
private:
    void issue_io() noexcept {
        // NOP（空操作）：立即成功，通过 on_sync_completion 手动 post 完成事件
        io_info_.result = 0;
        // Post completion manually — nop is synchronous, not overlapped
        // 手动 post 完成事件 — nop 是同步的，不是重叠 I/O
        op_->on_sync_completion(
            static_cast<platform::iocp::iocp_proactor*>(
                this_thread.worker->proactor), 0);
        (void)op_.release();
    }
};

/// Windows timeout: delegates blocking wait to a background thread that
/// posts IOCP completion after the delay. The operation is kept alive by
/// the thread via raw pointer ownership transfer.
// Windows 超时：将阻塞等待委托给后台线程，延迟后通过 IOCP 投递完成事件。
// 操作对象的所有权通过裸指针转移给后台线程以保证其生命周期。
//
// 为什么需要后台线程？
//   Windows 没有像 Linux timerfd 或 io_uring timeout 这样的异步定时器机制，
//   IOCP 本身不直接支持超时。唯一的方案是启动一个后台线程调用 Sleep()，
//   然后通过 PostQueuedCompletionStatus 或 on_sync_completion 通知完成。
//   这虽然有一个线程创建的开销，但对于定时器操作来说可以接受。
struct win_timeout final : win_awaiter_base<win_timeout> {
    template<typename Rep, typename Period>
    explicit win_timeout(const std::chrono::duration<Rep, Period>& dur) noexcept
        : win_awaiter_base() {
        dur_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
        if (dur_ms_ < 1) dur_ms_ = 1;
    }

private:
    void issue_io() noexcept {
        auto* raw_op = op_.release();        // transfer ownership to background thread
        // 通过 release 转移 ownership 到后台线程
        auto* proactor = static_cast<platform::iocp::iocp_proactor*>(
            this_thread.worker->proactor);
        DWORD ms = static_cast<DWORD>(dur_ms_);
        // Background thread: sleep → signal IOCP completion (via on_sync_completion)
        // 后台线程：休眠指定时间 → 通过 on_sync_completion 通知 IOCP 完成
        std::thread([raw_op, proactor, ms]() noexcept {
            Sleep(ms);
            raw_op->on_sync_completion(proactor, 0);
        }).detach();
    }

    long long dur_ms_ = 0;
};

/// Windows async read (file/pipe/console): uses background thread + _read + IOCP.
// Windows 异步读（文件/管道/控制台）：使用后台线程 + _read + IOCP。
//
// 为什么需要后台线程？
//   IOCP 原生只支持 socket 和 named pipe 的异步 I/O。
//   普通文件的异步 I/O 需要特定的文件标志（FILE_FLAG_OVERLAPPED）以及正确的设备类型支持。
//   对于不支持重叠 I/O 的文件描述符（如通过 _open 打开的文件），
//   只能使用后台线程模拟异步。
//
//   使用后台线程 + _read + IOCP 完成回调的模式：
//   后台线程执行阻塞的 _read，完成后通过 on_sync_completion 通知原始线程。
struct win_read final : win_awaiter_base<win_read> {
    win_read(int fd, std::span<char> buf, uint64_t /*offset*/) noexcept
        : win_awaiter_base() { fd_ = fd; buf_ = buf; }
private:
    void issue_io() noexcept {
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
// Windows 异步写（文件/管道/控制台）：使用后台线程 + _write + IOCP。
struct win_write final : win_awaiter_base<win_write> {
    win_write(int fd, std::span<const char> buf, uint64_t /*offset*/) noexcept
        : win_awaiter_base() { fd_ = fd; buf_ = buf; }
private:
    void issue_io() noexcept {
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

struct win_shutdown final : win_awaiter_base<win_shutdown> {
    win_shutdown(uintptr_t sock, int how) noexcept : win_awaiter_base() {
        sock_ = sock; how_ = how;
    }
private:
    void issue_io() noexcept {
        // shutdown 是同步操作，没有重叠 I/O 版本。
        // 与 closesocket 一样，通过 on_sync_completion 手动 post 完成事件。
        int ret = ::shutdown((SOCKET)sock_, how_);
        io_info_.result = (ret == 0) ? 0 : -::WSAGetLastError();
        // Post completion manually — shutdown is synchronous, not overlapped
        // 手动 post 完成事件 — shutdown 是同步的，不是重叠 I/O
        op_->on_sync_completion(
            static_cast<platform::iocp::iocp_proactor*>(
                this_thread.worker->proactor), 0);
        (void)op_.release();
    }
    int how_ = 0;
};

} // namespace coronet::detail

// Platform factory functions — uniform interface for async_io.hpp
// 平台工厂函数 — 为 async_io.hpp 提供统一接口
// 与 io_uring 版本相同，通过 platform_io 命名空间导出工厂函数
// 注意：Windows 的文件描述符在 win_read/win_write 中直接使用 int fd，
// 而 socket 操作使用 uintptr_t（因为 Windows SOCKET 是 UINT_PTR 类型）。
// 这与 Linux 平台不同（Linux 上 socket fd 和文件 fd 都是 int）。
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
