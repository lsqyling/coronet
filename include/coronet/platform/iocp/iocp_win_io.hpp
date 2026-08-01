#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include "coronet/detail/task_info.hpp"
#include "coronet/detail/thread_meta.hpp"
#include "coronet/detail/worker_meta.hpp"
#include "coronet/platform/iocp/iocp_proactor.hpp"

#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <queue>
#include <span>
#include <thread>
#include <vector>
#include <winsock2.h>
#include <mswsock.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <timeapi.h>   // timeBeginPeriod (winmm.lib)
#include <io.h>        // _read, _write

namespace coronet::detail {

// ============================================================
// win_chain_base — non-template base for typed chain dispatch
// ============================================================
// win_chain_base — 链式调用的非模板基类，提供类型化分发
//
// win_awaiter_base<Derived> 继承自此基类，在构造时将 chain_dispatch_fn
// 设置为 per-type 静态分发函数（编译期已知完整 Derived 类型）。
//
// 与旧方案（task_info 中存储 chain_fn + chain_ctx void* 对）相比：
//   1. 分发函数指针存储在 awaiter 自身（类型化上下文），而非通用 task_info
//   2. 编译器在 CRTP 实例化时已知完整类型，可优化静态调用
//   3. task_info 只需一个 chain_target 指针，节省 8 字节
//   4. 消除了每次 operator&& 迭代时创建 lambda 的开销
//
// win_awaiter_base<Derived> inherits from this base. At construction,
// chain_dispatch_fn is set to the per-type static dispatch function
// where the complete Derived type is known at compile time.
struct win_chain_base {
    using dispatch_fn_t = void (*)(win_chain_base* self) noexcept;

    dispatch_fn_t chain_dispatch_fn{nullptr};

    /// 调用链中下一个 awaiter 的 issue_io()，使用编译期确定的完整类型
    void chain_issue_next() noexcept { chain_dispatch_fn(this); }
};

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
class win_awaiter_base : public win_chain_base {
public:
    [[nodiscard]] int32_t result() const noexcept { return io_info_.result; }
    static constexpr bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> current) noexcept {
        io_info_.handle = current;
        io_info_.result = 0;
        auto* p = static_cast<platform::iocp::iocp_proactor*>(
            this_thread.worker->proactor);
        p->work_started();
        // 递增 inflight 操作计数，与 io_uring/epoll 后端保持一致
        // Increment inflight operation count, consistent with io_uring/epoll backends
        ++this_thread.worker->requests_to_reap;
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
        // 设置 per-type 链式分发函数。chain_dispatch_fn 中的 static_cast<Derived*>
        // 在 CRTP 实例化时确定完整类型，编译器可为每种 IO 类型生成内联版本。
        // Set per-type chain dispatch — static_cast<Derived*> is resolved at
        // CRTP instantiation time with the complete type known to the compiler.
        chain_dispatch_fn = [](win_chain_base* self) noexcept {
            static_cast<Derived*>(self)->issue_io();
        };
        // 从 proactor 获取（或回收复用）一个 iocp_operation
        auto* p = static_cast<platform::iocp::iocp_proactor*>(
            this_thread.worker->proactor);
        op_ = p->acquire_operation();
        if (op_) op_->set_user_data(io_info_.as_user_data());
    }

    // 注意：不再有纯虚函数 issue_io()。
    // 派生类提供 issue_io() 方法，通过 CRTP 在编译期分派。
    // 如果派生类忘记实现，在链接时会得到"未定义引用"错误。

    // P1-3 fix: errcode 参数允许调用方传入已保存的 WSAGetLastError 值，
    // 避免 closesocket 等中间调用覆盖错误码。errcode=0 表示由 finish_issue
    // 内部读取 WSAGetLastError（适用于 WSA 调用后立即调用的场景）。
    //
    // errcode parameter lets the caller pass a saved WSAGetLastError value,
    // preventing closesocket or other intervening calls from overwriting it.
    // errcode=0 means finish_issue reads WSAGetLastError itself (for callers
    // that call finish_issue immediately after the WSA function).
    void finish_issue(DWORD ioresult, DWORD /*bytes*/, DWORD errcode = 0) noexcept {
        // P1-2 fix: 传递 HANDLE（内核句柄）而非 iocp_proactor*（内存指针）。
        // finish_issue 在事件循环线程调用，proactor 保证存活，但为一致性统一用 HANDLE。
        auto* p = static_cast<platform::iocp::iocp_proactor*>(
            this_thread.worker->proactor);
        HANDLE iocp = reinterpret_cast<HANDLE>(p->native_handle());
        if (ioresult == 0) {
            op_->on_pending(iocp);
        } else {
            DWORD err = (errcode != 0) ? errcode : ::WSAGetLastError();
            if (err == WSA_IO_PENDING) {
                op_->on_pending(iocp);
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
    friend class win_awaiter_base<win_recv>;

    win_recv(uintptr_t sock, std::span<char> buf, int flags = 0) noexcept
        : win_awaiter_base() { sock_ = sock; buf_ = buf; flags_ = flags; }

private:
    void issue_io() noexcept {
        // WSARecv 是 Windows 的异步 socket 接收 API。
        // WSABUF 是 Windows 的 scatter/gather I/O 缓冲区描述符。
        WSABUF wbuf{.len = static_cast<ULONG>(buf_.size()), .buf = buf_.data()};
        DWORD fl = flags_, bytes = 0;
        int ret = ::WSARecv(static_cast<SOCKET>(sock_), &wbuf, 1, &bytes, &fl,
            static_cast<OVERLAPPED*>(op_->native_overlapped()), nullptr);
        finish_issue(ret, bytes);
    }
    std::span<char> buf_;
    int flags_ = 0;
};

struct win_send final : win_awaiter_base<win_send> {
    friend class win_awaiter_base<win_send>;

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
        int ret = ::WSASend(static_cast<SOCKET>(sock_), &wbuf_, 1, &bytes, flags_,
            static_cast<OVERLAPPED*>(op_->native_overlapped()), nullptr);
        finish_issue(ret, bytes);
    }
    WSABUF wbuf_{};
    int flags_ = 0;
};

struct win_accept final : win_awaiter_base<win_accept> {
    friend class win_awaiter_base<win_accept>;

    win_accept(uintptr_t sock, struct sockaddr* addr = nullptr,
               socklen_t* addrlen = nullptr, int flags = 0) noexcept
        : win_awaiter_base() {
        sock_ = sock; addr_ = addr; addrlen_ = addrlen; (void)flags;
        create_accept_socket();
    }

    [[nodiscard]] int32_t await_resume() const noexcept {
        // AcceptEx 完成后，必须调用 SO_UPDATE_ACCEPT_CONTEXT 使新 socket
        // 继承监听 socket 的属性（getsockname / getpeername / shutdown 等依赖此设置）。
        // 不调用此设置，send/recv 在多数场景下仍可工作，但某些配置下可能行为异常。
        //
        // After AcceptEx completes, SO_UPDATE_ACCEPT_CONTEXT must be called
        // so the accepted socket inherits the listen socket's properties
        // (getsockname, getpeername, shutdown, etc. depend on this).
        SOCKET listen_sock = static_cast<SOCKET>(sock_);
        ::setsockopt(static_cast<SOCKET>(accept_socket_), SOL_SOCKET,
                     SO_UPDATE_ACCEPT_CONTEXT,
                     reinterpret_cast<const char*>(&listen_sock),
                     sizeof(listen_sock));
        // await_resume 返回 accept 的新 socket 句柄（而非字节数）
        // 这是 IOCP 特有的：AcceptEx 需要预先创建 accept socket
        return static_cast<int32_t>(accept_socket_);
    }

private:
    void create_accept_socket() noexcept {
        // AcceptEx 要求预先创建一个 socket 用于接受连接。
        // 使用 WSA_FLAG_OVERLAPPED 标志使其支持重叠 I/O。
        //
        // P0-4 fix: AcceptEx 要求 accept socket 的地址族与 listen socket 一致。
        // 之前硬编码 AF_INET，IPv6 监听时 AcceptEx 必然失败。
        // 现在通过 getsockname 查询 listen socket 的地址族。
        int family = AF_INET;  // 默认 IPv4
        struct sockaddr_storage ss;
        int len = sizeof(ss);
        if (::getsockname(static_cast<SOCKET>(sock_),
                          reinterpret_cast<struct sockaddr*>(&ss), &len) == 0) {
            family = ss.ss_family;
        }

        accept_socket_ = ::WSASocketW(family, SOCK_STREAM, 0,
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
            // P1-3 fix: 传入 errcode=1（通用错误），避免 finish_issue 读 stale WSAGetLastError
            finish_issue(1, 0, 1);
            return;
        }

        auto* p = static_cast<platform::iocp::iocp_proactor*>(
            this_thread.worker->proactor);
        p->register_handle(sock_);

        DWORD addr_buf_len = sizeof(sockaddr_storage) + 16;
        memset(addr_buf_, 0, sizeof(addr_buf_));
        DWORD bytes_received = 0;
        BOOL ok = fn(static_cast<SOCKET>(sock_), accept_socket_, addr_buf_,
            0, addr_buf_len, addr_buf_len, &bytes_received,
            static_cast<OVERLAPPED*>(op_->native_overlapped()));

        if (!ok) {
            DWORD err = ::WSAGetLastError();  // 读取 AcceptEx 的错误码
            if (err == WSA_IO_PENDING) {
                op_->on_pending(p);
                op_.release();
            } else {
                ::closesocket(accept_socket_);  // ← 可能覆盖 WSAGetLastError
                if (err == ERROR_CONNECTION_ABORTED) {
                    // 连接被中止：重试
                    create_accept_socket();
                    if (accept_socket_ != INVALID_SOCKET) {
                        issue_io();
                        return;
                    }
                }
                accept_socket_ = INVALID_SOCKET;
                // P1-3 fix: 传入已保存的 err，避免 closesocket 覆盖 WSAGetLastError。
                // finish_issue 内部会用 err 设置 io_info_.result 并 post_completion。
                // 不再需要手动设 io_info_.result（finish_issue 会设）。
                // 不再需要手动 op_.release()（finish_issue 会 release）。
                finish_issue(1, 0, err);
            }
        } else {
            // 同步完成（罕见但可能）
            op_->Internal = 0;
            op_->InternalHigh = bytes_received;
            finish_issue(0, bytes_received);
            // finish_issue 内部已调用 op_.release()，无需再次释放
        }
    }

    struct sockaddr* addr_ = nullptr;
    socklen_t* addrlen_ = nullptr;
    uintptr_t accept_socket_ = coronet::platform::invalid_socket;
    char addr_buf_[sizeof(sockaddr_storage) * 2 + 32]{};
};

struct win_connect final : win_awaiter_base<win_connect> {
    friend class win_awaiter_base<win_connect>;

    win_connect(uintptr_t sock, const struct sockaddr* addr,
                socklen_t addrlen) noexcept
        : win_awaiter_base() { sock_ = sock; addr_ = addr; addrlen_ = addrlen; }

private:
    void issue_io() noexcept {
        LPFN_CONNECTEX fn = get_connect_ex();
        if (!fn) { finish_issue(1, 0, 1); return; }  // P1-3: pass errcode=1

        auto* p = static_cast<platform::iocp::iocp_proactor*>(
            this_thread.worker->proactor);
        p->register_handle(sock_);

        // ConnectEx 要求 socket 必须先 bind
        struct sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port = 0;
        ::bind(static_cast<SOCKET>(sock_), reinterpret_cast<struct sockaddr*>(&local),
               sizeof(local));

        BOOL ok = fn(static_cast<SOCKET>(sock_), addr_, addrlen_, nullptr, 0, nullptr,
            static_cast<OVERLAPPED*>(op_->native_overlapped()));

        if (!ok) {
            DWORD err = ::WSAGetLastError();
            if (err == WSA_IO_PENDING) {
                op_->on_pending(p);
                op_.release();
            } else {
                // P1-3 fix: 传入已保存的 err，避免 finish_issue 读 stale WSAGetLastError
                finish_issue(1, 0, err);
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
    friend class win_awaiter_base<win_close>;

    explicit win_close(uintptr_t sock) noexcept : win_awaiter_base() { sock_ = sock; }
private:
    void issue_io() noexcept {
        ::closesocket(static_cast<SOCKET>(sock_));
        io_info_.result = 0;
        // P1-2 fix: 传递 HANDLE 而非 proactor*
        HANDLE iocp = reinterpret_cast<HANDLE>(
            static_cast<platform::iocp::iocp_proactor*>(
                this_thread.worker->proactor)->native_handle());
        op_->on_sync_completion(iocp, 0);
        (void)op_.release();
    }
};

struct win_nop final : win_awaiter_base<win_nop> {
    friend class win_awaiter_base<win_nop>;

    win_nop() noexcept : win_awaiter_base() {}
private:
    void issue_io() noexcept {
        io_info_.result = 0;
        HANDLE iocp = reinterpret_cast<HANDLE>(
            static_cast<platform::iocp::iocp_proactor*>(
                this_thread.worker->proactor)->native_handle());
        op_->on_sync_completion(iocp, 0);
        (void)op_.release();
    }
};

// ============================================================
// win_file_io_pool — shared thread pool for blocking file I/O
// ============================================================
// Shared thread pool for Windows file I/O operations that cannot use
// overlapped I/O (e.g., files opened with _open, _read, _write).
//
// Replaces per-operation std::thread().detach() which creates a new
// thread per I/O call — a performance anti-pattern under high concurrency
// (5GB file / 1MB blocks = 5000 thread creations).
//
// Thread count: max(4, hardware_concurrency()).
// Work items are type-erased via std::function; typical captures (~48 bytes)
// fit within MSVC's SBO (Small Buffer Optimization), avoiding heap allocation.
//
// 与 epoll 后端的 file_io_pool 设计对称：
//   epoll:  file_io_pool (4 threads) + pipe2/eventfd 通知
//   IOCP:   win_file_io_pool (N threads) + IOCP 完成通知
//   io_uring: 内核原生异步，零线程
class win_file_io_pool {
public:
    // Leaky singleton: pool is never destroyed. This avoids use-after-free
    // during static destruction — pool workers may still be executing when
    // other statics (io_context, proactor) are destroyed. The OS reclaims
    // all resources on process exit.
    //
    // 泄漏式单例：池永不析构。避免静态析构期间 use-after-free —— 池 worker
    // 可能在其他静态对象（io_context, proactor）析构时仍在执行。
    // 进程退出时由 OS 回收所有资源。
    static win_file_io_pool& instance() noexcept {
        static auto* pool = new win_file_io_pool();
        return *pool;
    }

    template<typename F>
    void submit(F&& work) noexcept {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            queue_.emplace_back(std::forward<F>(work));
        }
        cv_.notify_one();
    }

    win_file_io_pool(const win_file_io_pool&) = delete;
    win_file_io_pool& operator=(const win_file_io_pool&) = delete;

private:
    win_file_io_pool() noexcept {
        unsigned n = std::thread::hardware_concurrency();
        if (n < 4) n = 4;
        for (unsigned i = 0; i < n; ++i) {
            threads_.emplace_back([this] { worker(); });
        }
    }

    // Destructor never runs (leaky singleton). Defined for completeness.
    ~win_file_io_pool() = default;

    void worker() {
        while (true) {
            std::function<void()> work;
            {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                work = std::move(queue_.front());
                queue_.pop_front();
            }
            work();
        }
    }

    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
};

// ============================================================
// win_timer_thread — dedicated timer thread with deadline min-heap
// ============================================================
// Windows 没有 Linux timerfd / io_uring timeout 那样的内核定时器，
// 只能借助线程等待模拟超时。
//
// 旧方案（bug #1）：把 Sleep(ms) 丢给共享线程池 win_file_io_pool。
//   当并发 Sleep 数超过池线程数（max(4, hardware_concurrency())）时，
//   后提交的短超时会被长超时堵在 FIFO 队列里（队头阻塞），
//   完成顺序不再等于截止时间顺序 —— combinator_stress Phase 5 失败根因
//   （150ms 超时排队等到 100ms 完成后才启动，反而晚于 200ms 超时）。
//
// 旧方案（bug #2）：条件变量 condition_variable::wait_until + notify_one。
//   在 MSVC 14.41 上实测（mini_cv 独立复现）：worker 阻塞在
//   wait_until(长截止时间) 时，notify_one 无法将其唤醒，短超时会一直
//   等到旧的长截止时间才批量触发（combinator_stress 每阶段 10s 的根因）。
//
// 当前方案：单一专用定时器线程 + 最小堆 + Win32 waitable timer + pulse 事件。
//   - submit(): 加锁把 (deadline, seq, op, iocp) 推入最小堆，
//     将 waitable timer 重新武装到最早的 deadline，SetEvent(pulse) 唤醒 worker
//   - worker 循环：弹出所有已到期条目（堆序弹出 → 完成顺序 == 截止顺序），
//     逐个 op->on_sync_completion(iocp, 0) 投递到 IOCP；
//     无到期条目时 WaitForMultipleObjects(timer, pulse) 阻塞
//   - timer 负责"无人提交时按时到期"，pulse 负责"新提交/更早 deadline 立即醒来"，
//     两者互不依赖，语义确定
//   - 完成顺序 == 截止时间顺序（与调度抖动无关），
//     与 Linux 内核定时器（timerfd / IORING_OP_TIMEOUT）语义一致
//   - 1 个线程服务任意数量的超时，无队头阻塞
//
// 精度（bug #3）：Windows 系统定时器分辨率默认 15.625ms（64 tick/s），
//   SetWaitableTimer 的到期时间被量化到下一个系统 tick，触发误差 0~15.6ms
//   （timer_accuracy 实测 late 2.6~14.5ms，Linux timerfd 为亚毫秒）。
//   构造时调用 timeBeginPeriod(1) 将 tick 提升到 1ms，触发误差降至 ~1ms。
//   副效果：deinit drain 中 GQCS(1ms) 每轮实际耗时从 ~10-15.6ms 变为 ~1ms。
//
// Leaky singleton：永不析构（与 win_file_io_pool 一致）。定时器线程在
// 进程退出时由 OS 回收。向已关闭的 IOCP handle 投递完成事件是良定义行为
// （PostQueuedCompletionStatus 返回 0），操作对象被回收，不泄漏。
// timeBeginPeriod(1) 仅在构造时调用一次、不配对的 timeEndPeriod ——
// 进程存活期间保持 1ms 分辨率（node.js / boost.asio 通行做法），
// 系统计数器在进程退出时自动清零，无需手动恢复。
class win_timer_thread {
public:
    static win_timer_thread& instance() noexcept {
        static auto* timer = new win_timer_thread();
        return *timer;
    }

    /// 提交一个超时：deadline = now + ms，到期后投递完成事件到 iocp。
    /// op 的所有权转移给定时器线程（裸指针传递，完成后由事件循环回收）。
    void submit(platform::iocp::iocp_operation* op, HANDLE iocp,
                long long ms) noexcept {
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(ms);
        {
            std::lock_guard<std::mutex> lock(mtx_);
            heap_.push(entry{deadline, seq_++, op, iocp});
            arm_timer_locked();      // 重新武装到最早 deadline
        }
        SetEvent(pulse_);            // 确保 worker 立即醒来处理新条目
    }

    win_timer_thread(const win_timer_thread&) = delete;
    win_timer_thread& operator=(const win_timer_thread&) = delete;

private:
    struct entry {
        std::chrono::steady_clock::time_point deadline;
        uint64_t seq;                              // 同 deadline 时的提交顺序
        platform::iocp::iocp_operation* op;
        HANDLE iocp;

        // 最小堆：std::priority_queue 是最大堆，反转比较得到小顶堆。
        // 先按 deadline，同 deadline 时先提交的（seq 小）先弹出。
        bool operator<(const entry& other) const noexcept {
            if (deadline != other.deadline) return deadline > other.deadline;
            return seq > other.seq;
        }
    };

    // 必须在持有 mtx_ 时调用。把 waitable timer 武装到堆顶 deadline
    // （相对时间，100ns 单位，负数表示相对当前时刻）。
    void arm_timer_locked() noexcept {
        if (heap_.empty()) {
            CancelWaitableTimer(timer_);
            return;
        }
        auto rel_us = std::chrono::duration_cast<std::chrono::microseconds>(
            heap_.top().deadline - std::chrono::steady_clock::now()).count();
        if (rel_us < 0) rel_us = 0;
        LARGE_INTEGER due;
        due.QuadPart = -rel_us * 10;   // 相对时间：-N × 100ns
        SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
    }

    win_timer_thread() noexcept
        : pulse_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),  // auto-reset
          timer_(CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS)),
          thread_([this] { worker(); }) {
        // bug #3: 提升系统定时器分辨率 15.625ms → 1ms。
        // SetWaitableTimer 的到期时间被量化到下一个系统 tick，默认 64Hz 下
        // 触发误差达 0~15.6ms（timer_accuracy 实测 late 2.6~14.5ms）。
        // timeBeginPeriod(1) 使 tick 变为 1ms，触发误差降至 ~1ms；
        // 同时使 deinit drain 中 GQCS(1ms) 每轮实际耗时 ~10-15.6ms → ~1ms。
        // 泄漏式单例 → 进程存活期间保持 1ms，不调 timeEndPeriod
        // （node.js / boost.asio 通行做法，进程退出时 OS 自动清零计数器）。
        ::timeBeginPeriod(1);
    }

    void worker() noexcept {
        std::vector<entry> expired;
        HANDLE handles[2] = {timer_, pulse_};
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mtx_);
                auto now = std::chrono::steady_clock::now();
                // 弹出所有已到期条目（堆序弹出 → 完成顺序 == 截止顺序）
                while (!heap_.empty() && heap_.top().deadline <= now) {
                    expired.push_back(heap_.top());
                    heap_.pop();
                }
                if (!heap_.empty()) arm_timer_locked();  // 重武装到新的最早 deadline
            }
            if (expired.empty()) {
                // 无到期条目 → 等待 timer 到期 或 submit 的 pulse
                WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            } else {
                for (auto& e : expired) {
                    e.op->on_sync_completion(e.iocp, 0);
                }
                expired.clear();
            }
        }
    }

    std::mutex mtx_;
    std::priority_queue<entry> heap_;
    uint64_t seq_ = 0;             // 仅锁内访问
    HANDLE pulse_ = nullptr;       // auto-reset 事件：submit 时 SetEvent
    HANDLE timer_ = nullptr;       // waitable timer：武装到最早 deadline
    std::thread thread_;
};

/// Windows timeout: delegates waiting to the dedicated timer thread
/// that posts IOCP completion after the deadline. The operation is
/// kept alive by the timer thread via raw pointer ownership transfer.
// Windows 超时：将等待委托给专用定时器线程，到点后通过 IOCP 投递完成事件。
// 操作对象的所有权通过裸指针转移给定时器线程以保证其生命周期。
//
// 为什么用专用定时器线程？
//   Windows 没有像 Linux timerfd 或 io_uring timeout 这样的异步定时器机制，
//   IOCP 本身不直接支持超时。唯一的方案是在后台线程中等待 Sleep()，
//   然后通过 on_sync_completion 通知完成。
//   旧实现把 Sleep 丢给共享文件 I/O 线程池，超时数超过池线程数时会发生
//   队头阻塞导致完成顺序错乱（见 win_timer_thread 注释）。专用定时器线程
//   用最小堆按截止时间排序，保证完成顺序与截止时间一致，且任意数量的
//   超时都只需一个线程。
struct win_timeout final : win_awaiter_base<win_timeout> {
    friend class win_awaiter_base<win_timeout>;

    template<typename Rep, typename Period>
    explicit win_timeout(const std::chrono::duration<Rep, Period>& dur) noexcept
        : win_awaiter_base() {
        dur_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
        if (dur_ms_ < 1) dur_ms_ = 1;
    }

private:
    void issue_io() noexcept {
        auto* raw_op = op_.release();        // transfer ownership to timer thread
        // P1-2 fix: 捕获 HANDLE（内核句柄）而非 proactor*（内存指针）。
        // HANDLE 是值语义，io_context 析构后 PostQueuedCompletionStatus
        // 对已关闭 handle 返回 0（良定义行为），彻底消除 use-after-free。
        HANDLE iocp = reinterpret_cast<HANDLE>(
            static_cast<platform::iocp::iocp_proactor*>(
                this_thread.worker->proactor)->native_handle());
        DWORD ms = static_cast<DWORD>(dur_ms_);
        win_timer_thread::instance().submit(raw_op, iocp, ms);
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
    friend class win_awaiter_base<win_read>;

    win_read(int fd, std::span<char> buf, uint64_t offset) noexcept
        : win_awaiter_base() { fd_ = fd; buf_ = buf; offset_ = offset; }
private:
    void issue_io() noexcept {
        auto* raw_op = op_.release();
        // P1-2 fix: 捕获 HANDLE 而非 proactor*
        HANDLE iocp = reinterpret_cast<HANDLE>(
            static_cast<platform::iocp::iocp_proactor*>(
                this_thread.worker->proactor)->native_handle());
        int f = fd_; auto sp = buf_; uint64_t off = offset_;
        win_file_io_pool::instance().submit(
            [raw_op, iocp, f, sp, off]() noexcept {
                if (off != uint64_t(-1)) {
                    _lseeki64(f, static_cast<__int64>(off), SEEK_SET);
                }
                int n = ::_read(f, sp.data(), static_cast<unsigned>(sp.size()));
                raw_op->on_sync_completion(iocp, (n >= 0) ? static_cast<DWORD>(n) : 0);
            });
    }
    int fd_ = 0;
    std::span<char> buf_;
    uint64_t offset_ = uint64_t(-1);
};

/// Windows async write (file/pipe/console): uses background thread + _write + IOCP.
// Windows 异步写（文件/管道/控制台）：使用后台线程 + _write + IOCP。
struct win_write final : win_awaiter_base<win_write> {
    friend class win_awaiter_base<win_write>;

    win_write(int fd, std::span<const char> buf, uint64_t offset) noexcept
        : win_awaiter_base() { fd_ = fd; buf_ = buf; offset_ = offset; }
private:
    void issue_io() noexcept {
        auto* raw_op = op_.release();
        // P1-2 fix: 捕获 HANDLE 而非 proactor*
        HANDLE iocp = reinterpret_cast<HANDLE>(
            static_cast<platform::iocp::iocp_proactor*>(
                this_thread.worker->proactor)->native_handle());
        int f = fd_; auto sp = buf_; uint64_t off = offset_;
        win_file_io_pool::instance().submit(
            [raw_op, iocp, f, sp, off]() noexcept {
                if (off != uint64_t(-1)) {
                    _lseeki64(f, static_cast<__int64>(off), SEEK_SET);
                }
                int n = ::_write(f, sp.data(), static_cast<unsigned>(sp.size()));
                raw_op->on_sync_completion(iocp, (n >= 0) ? static_cast<DWORD>(n) : 0);
            });
    }
    int fd_ = 0;
    std::span<const char> buf_;
    uint64_t offset_ = uint64_t(-1);
};

struct win_shutdown final : win_awaiter_base<win_shutdown> {
    friend class win_awaiter_base<win_shutdown>;

    win_shutdown(uintptr_t sock, int how) noexcept : win_awaiter_base() {
        sock_ = sock; how_ = how;
    }
private:
    void issue_io() noexcept {
        int ret = ::shutdown(static_cast<SOCKET>(sock_), how_);
        io_info_.result = (ret == 0) ? 0 : -::WSAGetLastError();
        // P1-2 fix: 传递 HANDLE 而非 proactor*
        HANDLE iocp = reinterpret_cast<HANDLE>(
            static_cast<platform::iocp::iocp_proactor*>(
                this_thread.worker->proactor)->native_handle());
        op_->on_sync_completion(iocp, 0);
        (void)op_.release();
    }
    int how_ = 0;
};

// ============================================================
// UDP operations: WSARecvFrom / WSASendTo
// ============================================================
// UDP recvfrom/sendto — 接收/发送数据报，同时获取/指定对端地址。
// 与 TCP 的 recv/send 不同，UDP 是无连接的，每个数据报可以来自/发往不同地址。
//
// WSARecvFrom: 异步接收数据报，完成后将源地址写入 addr_storage_。
// WSASendTo:   异步发送数据报到指定目标地址。
//
// recvfrom_result 返回 {bytes, sockaddr_storage}，由 udp_socket 层
// 包装为 inet_address。

/// recvfrom 操作的返回类型 — 字节数 + 源地址。
struct recvfrom_result {
    int32_t bytes;
    struct sockaddr_storage addr;
};

struct win_recvfrom final : win_awaiter_base<win_recvfrom> {
    friend class win_awaiter_base<win_recvfrom>;

    win_recvfrom(uintptr_t sock, std::span<char> buf, int flags = 0) noexcept
        : win_awaiter_base() {
        sock_ = sock; buf_ = buf; flags_ = flags;
        addr_len_ = sizeof(addr_storage_);
    }

    /// 覆盖基类的 await_resume — 返回 recvfrom_result 而非 int32_t。
    [[nodiscard]] recvfrom_result await_resume() const noexcept {
        return recvfrom_result{io_info_.result, addr_storage_};
    }

private:
    void issue_io() noexcept {
        WSABUF wbuf{.len = static_cast<ULONG>(buf_.size()), .buf = buf_.data()};
        DWORD fl = static_cast<DWORD>(flags_), bytes = 0;
        int ret = ::WSARecvFrom(static_cast<SOCKET>(sock_), &wbuf, 1, &bytes, &fl,
            reinterpret_cast<struct sockaddr*>(&addr_storage_), &addr_len_,
            static_cast<OVERLAPPED*>(op_->native_overlapped()), nullptr);
        finish_issue(ret, bytes);
    }
    std::span<char> buf_;
    int flags_ = 0;
    struct sockaddr_storage addr_storage_{};
    int addr_len_ = 0;
};

struct win_sendto final : win_awaiter_base<win_sendto> {
    friend class win_awaiter_base<win_sendto>;

    win_sendto(uintptr_t sock, std::span<const char> buf,
               const struct sockaddr* addr, socklen_t addrlen,
               int flags = 0) noexcept
        : win_awaiter_base() {
        sock_ = sock;
        wbuf_ = WSABUF{.len = static_cast<ULONG>(buf.size()),
                        .buf = const_cast<char*>(buf.data())};
        addr_ = addr; addrlen_ = addrlen; flags_ = flags;
    }

private:
    void issue_io() noexcept {
        DWORD bytes = 0;
        int ret = ::WSASendTo(static_cast<SOCKET>(sock_), &wbuf_, 1, &bytes,
            static_cast<DWORD>(flags_), addr_, addrlen_,
            static_cast<OVERLAPPED*>(op_->native_overlapped()), nullptr);
        finish_issue(ret, bytes);
    }
    WSABUF wbuf_{};
    const struct sockaddr* addr_ = nullptr;
    socklen_t addrlen_ = 0;
    int flags_ = 0;
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
        { return win_recv{static_cast<uintptr_t>(fd), buf, flags}; }
    inline auto make_send(int fd, std::span<const char> buf, int flags) noexcept
        { return win_send{static_cast<uintptr_t>(fd), buf, flags}; }
    inline auto make_accept(int fd, struct sockaddr* a, socklen_t* al, int fl) noexcept
        { return win_accept{static_cast<uintptr_t>(fd), a, al, fl}; }
    inline auto make_connect(int fd, const struct sockaddr* a, socklen_t al) noexcept
        { return win_connect{static_cast<uintptr_t>(fd), a, al}; }
    inline auto make_close(int fd) noexcept
        { return win_close{static_cast<uintptr_t>(fd)}; }
    inline auto make_shutdown(int fd, int how) noexcept
        { return win_shutdown{static_cast<uintptr_t>(fd), how}; }
    inline auto make_recvfrom(int fd, std::span<char> buf, int flags) noexcept
        { return win_recvfrom{static_cast<uintptr_t>(fd), buf, flags}; }
    inline auto make_sendto(int fd, std::span<const char> buf,
                            const struct sockaddr* a, socklen_t al, int fl) noexcept
        { return win_sendto{static_cast<uintptr_t>(fd), buf, a, al, fl}; }
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
