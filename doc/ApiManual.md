# coronet API 手册

> 最新更新: 2026-07-27

## 目录

- [一、核心类型](#一核心类型)
  - [1.1 io_context — 事件循环](#11-io_context--事件循环)
  - [1.2 task\<T\> — 惰性协程任务](#12-taskt--惰性协程任务)
  - [1.3 shared_task\<T\> — 多等待者](#13-shared_taskt--多等待者)
  - [1.4 generator\<T\> — 生成器](#14-generatort--生成器)
- [二、异步 I/O](#二异步-io)
  - [2.1 Socket I/O](#21-socket-io)
  - [2.2 File I/O](#22-file-io)
  - [2.3 控制](#23-控制)
  - [2.4 链式 I/O](#24-链式-io)
- [三、网络模块](#三网络模块)
  - [3.1 socket — 跨平台 RAII 套接字](#31-socket--跨平台-raii-套接字)
  - [3.2 acceptor — TCP 监听器](#32-acceptor--tcp-监听器)
  - [3.3 inet_address](#33-inet_address)
- [四、TLS 模块](#四tls-模块)
  - [4.1 tls_context](#41-tls_context)
  - [4.2 tls_socket](#42-tls_socket)
  - [4.3 tls_acceptor](#43-tls_acceptor)
- [五、协程同步原语](#五协程同步原语)
  - [5.1 mutex — 协程互斥锁](#51-mutex--协程互斥锁)
  - [5.2 condition_variable](#52-condition_variable)
  - [5.3 counting_semaphore](#53-counting_semaphore)
  - [5.4 channel — CSP 通道](#54-channel--csp-通道)
  - [5.5 when_all / when_any / when_some — 组合器](#55-when_all--when_any--when_some--组合器)
  - [5.6 defer — RAII 作用域守卫](#56-defer--raii-作用域守卫)
- [六、设计决策与性能分析](#六设计决策与性能分析)
  - [6.1 架构总览](#61-架构总览)
  - [6.2 epoll 后端性能](#62-epoll-后端性能)
  - [6.3 io_uring 后端性能](#63-io_uring-后端性能)
  - [6.4 Windows IOCP 性能](#64-windows-iocp-性能)
  - [6.5 编译器性能对比](#65-编译器性能对比)
  - [6.6 链式 co_await](#66-链式-co_await)
  - [6.7 跨线程 co_spawn 架构](#67-跨线程-co_spawn-架构)
  - [6.8 关键 Bug 修复](#68-关键-bug-修复)
  - [6.9 构建与测试](#69-构建与测试)
- [API 速查表](#api-速查表)

---

## 一、核心类型

### 1.1 io_context — 事件循环

```cpp
#include <coronet/io_context.hpp>
```

核心事件循环。每个实例拥有一个平台 Proactor + 工作线程。多实例支持多线程 I/O。

```cpp
coronet::io_context ctx;

// 启动协程（立即返回，在 ctx 的线程上运行）
ctx.co_spawn(my_task());

// 启动事件循环（创建后台线程）
ctx.start();

// 通知停止并唤醒事件循环
ctx.can_stop();

// 等待事件循环线程退出
ctx.join();
```

**自由函数：**

```cpp
// 在当前线程的 io_context 上启动协程
void co_spawn(task<void>&& entrance) noexcept;

// 获取当前线程的 io_context
io_context& this_io_context() noexcept;
```

**生命周期：**

```
co_spawn(task)     → push to SPSC ring
ctx.start()        → create thread → run() loop
  loop: drain → work → submit → complete
ctx.can_stop()     → set will_stop_ flag + wakeup proactor
ctx.join()         → wait for thread exit
```

**多线程：** 创建 N 个 `io_context` 实例，将任务分配到各实例，全部启动，全部 join。

---

### 1.2 task\<T\> — 惰性协程任务

```cpp
#include <coronet/task.hpp>
```

Move-only、唯一所有权的协程任务。惰性执行 — 仅在 `co_await` 时运行协程体。

```cpp
task<int> compute() {
    co_return 42;
}

task<void> caller() {
    int v = co_await compute();  // compute 在此处启动并运行
    // ...
}
```

**关键特性：**

- `task<T>` — 通过 co_return 返回值
- `task<void>` — 无返回值
- `task<T&>` — 返回引用
- `initial_suspend()` → `suspend_always`（惰性启动）
- `final_suspend()` → 返回父级句柄（内联 chain）
- `detach()` → fire-and-forget（仅 task\<void\>）
- 不可拷贝，可移动

---

### 1.3 shared_task\<T\> — 多等待者

```cpp
#include <coronet/shared_task.hpp>

shared_task<int> compute() { co_return 42; }

auto st = compute();        // 启动执行
int a = co_await st;        // 第一个消费者
int b = co_await st;        // 第二个消费者（相同结果）
// 引用计数 — 最后一个引用消失时销毁
```

---

### 1.4 generator\<T\> — 生成器

```cpp
#include <coronet/generator.hpp>

coronet::generator<int> iota(int start) {
    while (true) {
        co_yield start++;
    }
}

// 与 ranges 搭配使用
for (auto x : iota(1) | std::views::take(5)) {
    std::cout << x << " ";  // 1 2 3 4 5
}
```

支持：值类型、引用类型、常量引用类型、自定义分配器、`elements_of()`。

---

## 二、异步 I/O

```cpp
#include <coronet/async_io.hpp>
```

所有函数位于 `coronet::async::` 命名空间。每个函数返回 awaitable。

### 2.1 Socket I/O

```cpp
// 接收数据到 span
auto n = co_await async::recv(fd, buf);             // → int
auto n = co_await async::recv(fd, buf, flags);      // 带 flags

// 从 span 发送数据
auto n = co_await async::send(fd, buf);             // → int
auto n = co_await async::send(fd, buf, flags);

// 接受连接
auto fd = co_await async::accept(listen_fd);        // → int
auto fd = co_await async::accept(listen_fd, &addr, &addrlen, flags);

// 连接
auto res = co_await async::connect(fd, &addr, addrlen);  // → int

// 关闭
co_await async::close(fd);

// 半关闭
co_await async::shutdown(fd, SHUT_RDWR);  // Linux: SHUT_RD/SHUT_WR/SHUT_RDWR
                                           // Win: SD_RECEIVE/SD_SEND/SD_BOTH
```

### 2.2 File I/O

Linux io_uring / Windows 后台线程实现。

```cpp
auto n = co_await async::read(fd, buf);               // → int
auto n = co_await async::read(fd, buf, file_offset);

auto n = co_await async::write(fd, buf);              // → int
auto n = co_await async::write(fd, buf, file_offset);
```

### 2.3 控制

```cpp
co_await async::yield();                      // 重新调度（nop）
co_await async::timeout(std::chrono::seconds{1});  // 相对延迟
co_await async::timeout_at(deadline);        // 绝对时间点
```

### 2.4 链式 I/O

```cpp
// 单次挂起执行 send + recv — io_uring 为内核级，epoll/IOCP 为用户态
int n = co_await (sock.send(pong) && sock.recv(buf));
```

---

## 三、网络模块

```cpp
#include <coronet/net.hpp>
```

### 3.1 socket — 跨平台 RAII 套接字

```cpp
coronet::socket sock{fd};  // 接管 fd/SOCKET 的所有权

// 通过 socket 异步 I/O
int n = co_await sock.recv(buf);
int n = co_await sock.send(buf);
int n = co_await sock.connect(addr);
co_await sock.close();
co_await sock.shutdown_write();

// 同步设置
sock.bind(addr).listen(SOMAXCONN);
sock.set_reuse_addr(true);
sock.set_tcp_no_delay(true);
sock.set_nonblocking();

// 信息
auto local = sock.local_addr();
auto peer  = sock.peer_addr();
auto fd    = sock.native_handle();

// 工厂方法
auto tcp = socket::create_tcp(AF_INET);
auto udp = socket::create_udp(AF_INET);

// RAII：析构函数在仍有有效句柄时调用 ::close()/closesocket()
```

### 3.2 acceptor — TCP 监听器

```cpp
coronet::acceptor ac{coronet::inet_address{port}};
// bind + listen 在构造函数中完成

int client_fd = co_await ac.accept();
```

### 3.3 inet_address

```cpp
// 仅端口
coronet::inet_address addr{8080};

// 主机名 + 端口（DNS 解析）
coronet::inet_address addr;
inet_address::resolve("example.com", 80, addr);

// 原始 sockaddr
coronet::inet_address addr{&sa, sizeof(sa)};

// Family
auto family = addr.family();  // AF_INET 或 AF_INET6
auto len    = addr.length();  // sizeof(sockaddr_in) 或 sockaddr_in6
```

---

## 四、TLS 模块

```cpp
#include <coronet/net/tls.hpp>
```

> **前置条件：** `CORONET_HAS_TLS` 必须定义，需要链接 OpenSSL。

TLS 模块在 tcp_socket 之上提供透明的加密通信层，采用 **BIO 桥接模式**：

```
  ┌──────────────────────────────────────────────────┐
  │                  tls_socket                       │
  │                                                   │
  │   用户明文          OpenSSL 加密         网络密文   │
  │   ┌──────┐    ┌─────────────────┐    ┌────────┐  │
  │   │ recv │ ←─ │ SSL_read (rbio) │ ←─ │tcp.recv│  │
  │   │ send │ ─→ │ SSL_write(wbio) │ ─→ │tcp.send│  │
  │   └──────┘    └─────────────────┘    └────────┘  │
  └──────────────────────────────────────────────────┘
```

异步 I/O 桥接：SSL_read/SSL_write 为同步调用，可能返回 WANT_READ/WANT_WRITE。若返回 WANT_READ，SSL 需要更多密文数据 → `co_await tcp.recv()` → `BIO_write(rbio)`；若返回 WANT_WRITE，SSL 已产生密文数据 → drain wbio → `co_await tcp.send()`。

### 4.1 tls_context

管理 OpenSSL SSL_CTX 对象的生命周期和配置。一个 tls_context 可供多个 tls_socket 共享。

| 方法 | 说明 |
|------|------|
| `tls_context(mode, version)` | 构造函数。mode: `client`/`server`，version: `tls12`/`tls13`/`tls12_and_13`（默认） |
| `load_cert_file(cert, key)` | 从 PEM 文件加载证书和私钥 |
| `load_cert_string(cert, key)` | 从内存字符串加载证书和私钥（PEM 格式） |
| `set_alpn(protocols)` | 设置 ALPN 协议列表（如 `{"h2", "http/1.1"}`）。客户端发送偏好列表，服务端从中选择 |
| `set_verify_peer(bool)` | 启用/禁用对端证书验证（客户端默认应启用） |
| `set_ca_file(path)` | 加载 CA 证书文件用于验证对端证书 |
| `set_default_verify_paths()` | 使用系统默认 CA 路径 |
| `native_handle()` | 获取底层 SSL_CTX 指针 |
| `is_server()` / `is_client()` | 查询模式 |

```cpp
// 服务端
tls_context ctx{tls_context::mode::server};
ctx.load_cert_file("server.crt", "server.key");
ctx.set_alpn({"h2", "http/1.1"});

// 客户端
tls_context ctx{tls_context::mode::client};
ctx.set_verify_peer(true);
ctx.set_ca_file("ca.pem");
ctx.set_alpn({"h2", "http/1.1"});
```

### 4.2 tls_socket

在 TCP 之上提供 TLS 加密。满足 transport concept。可移动，不可拷贝。

**生命周期：** 构造 → 握手（handshake/connect）→ 数据传输（recv/send）→ 关闭（close/close_graceful/shutdown_write）。

| 方法 | 说明 |
|------|------|
| `tls_socket(tcp_socket&&, const tls_context&)` | 从已连接的 TCP 套接字构造 |
| `static connect(addr, ctx)` | 客户端工厂：创建 TCP 套接字 → 连接 → TLS 握手，返回就绪的 tls_socket |
| `handshake()` | 服务端 TLS 握手（TCP accept 之后调用） |
| `recv(buf)` | 异步接收解密数据。返回 >0: 字节数，0: EOF，<0: 错误 |
| `send(buf)` | 异步发送加密数据。返回 >0: 字节数，<0: 错误 |
| `close()` | 快速关闭：发送 close_notify（不等待对端响应）→ 释放 SSL → 关闭 TCP |
| `close_graceful()` | 优雅关闭：发送 close_notify → 等待对端 close_notify → 关闭 TCP（慢于 close，但避免对端收到 RST 导致数据丢失） |
| `shutdown_write()` | 半关闭写端：发送 close_notify，通知对端发送完毕 |
| `negotiated_alpn()` | 获取握手后协商的 ALPN 协议（`std::optional<std::string>`） |
| `is_handshake_done()` | TLS 握手是否已完成 |
| `native_handle()` | 获取底层 SSL 会话句柄 |

```cpp
// 服务端（通过 tls_acceptor）
tls_acceptor ac{addr, ctx};
auto conn = co_await ac.accept_socket();
int n = co_await conn.recv(buf);
co_await conn.send(data);

// 客户端
auto sock = co_await tls_socket::connect(addr, ctx);
co_await sock.send(data);
int n = co_await sock.recv(buf);
```

### 4.3 tls_acceptor

TLS 连接接收器，包装 tcp_acceptor。满足 listener concept。

工作流程：tcp_acceptor 接受 TCP 连接 → 用 tls_context 创建 tls_socket → 执行服务端 TLS 握手 → 返回就绪的 tls_socket。

| 方法 | 说明 |
|------|------|
| `tls_acceptor(addr, ctx, backlog)` | 构造 TLS 接收器 |
| `accept_socket()` | 异步接受 TLS 连接，返回已完成握手的 tls_socket |
| `listen_fd()` | 获取底层监听套接字的原生句柄 |

```cpp
tls_context ctx{tls_context::mode::server};
ctx.load_cert_file("server.crt", "server.key");

tls_acceptor ac{inet_address{443}, ctx};
while (true) {
    auto conn = co_await ac.accept_socket();
    co_spawn(tls_session(std::move(conn)));
}
```

---

## 五、协程同步原语

### 5.1 mutex — 协程互斥锁

```cpp
#include <coronet/co/mutex.hpp>

coronet::mutex mtx;
int counter = 0;

task<> increment() {
    auto guard = co_await mtx.lock_guard();  // RAII
    ++counter;                                // 受保护
}   // 作用域退出时自动解锁

// 手动 lock/unlock
co_await mtx.lock();
mtx.unlock();
```

CAS 快速路径用于无竞争锁，链表等待队列用于有竞争情况。跨线程安全。

---

### 5.2 condition_variable

```cpp
coronet::condition_variable cv;
coronet::mutex mtx;
bool ready = false;

task<> waiter() {
    auto lk = co_await mtx.lock_guard();
    co_await cv.wait(mtx, [&] { return ready; });
    // 此时 ready == true
}

task<> notifier() {
    {
        auto lk = co_await mtx.lock_guard();
        ready = true;
    }
    cv.notify_one();   // 或 cv.notify_all()
}
```

---

### 5.3 counting_semaphore

```cpp
coronet::counting_semaphore sem{3};  // 最多 3 个并发

task<> worker(int id) {
    co_await sem.acquire();   // 等待槽位
    // ... 最多 3 个 worker 同时运行
    sem.release();            // 释放槽位
}
```

---

### 5.4 channel — CSP 通道

```cpp
coronet::channel<std::string, 8> ch;

// 生产者
task<> produce() {
    co_await ch.release("hello");
}

// 消费者
task<> consume() {
    std::string msg = co_await ch.acquire();
}
```

**容量模式：**

- `channel<T>`（默认，capacity=0）— 会合模式（生产者阻塞直到消费者就绪）
- `channel<T, 1>` — 单槽缓冲
- `channel<T, N>` — N 槽缓冲

---

### 5.5 when_all / when_any / when_some — 组合器

```cpp
#include <coronet/co/when_all.hpp>

// 等待所有任务，以 tuple 形式获取结果（void 被过滤）
auto [r0, r1] = co_await all(f0(), f1(), f2());

// 等待第一个任务完成
auto [idx, var] = co_await any(f0(), f1(), f2());

// 等待 N 个任务完成
auto results = co_await some(2, f0(), f1(), f2());

// std::visit 辅助
std::visit(overload{
    [](int x)            { /* handle int */ },
    [](const std::string& s) { /* handle string */ },
    [](std::monostate)   { /* handle void */ },
}, var);
```

---

### 5.6 defer — RAII 作用域守卫

```cpp
#include <coronet/utility/defer.hpp>

task<> example(int fd) {
    defer _{ [fd] { ::close(fd); } };  // 作用域退出时必定执行
    co_await async::write(fd, buf);
    // 即使抛出异常，fd 也会在此处关闭
}
```

---

## 六、设计决策与性能分析

### 6.1 架构总览

| 维度 | coronet | ASIO |
|------|---------|------|
| **编程范式** | C++20 协程 (`co_await`) | 回调 |
| **I/O 后端** | epoll / io_uring / IOCP | epoll / IOCP |
| **跨平台** | Windows + Linux | Windows + Linux |
| **多态** | 编译期 (CRTP + `#ifdef`，零虚表) | 编译期 (模板) |
| **Proactor** | 栈上具体类型，零堆分配 | — |
| **链式 I/O** | `co_await (recv && send)` — io_uring 内核级 / epoll&IOCP 用户态 | 嵌套回调 |
| **跨线程 spawn** | mutex queue + eventfd/PQCS 唤醒 | `post()` / `dispatch()` |

测试环境：

| 项目 | Linux | Windows |
|------|-------|---------|
| OS | WSL2 Ubuntu (Kernel 5.15) | Windows 10 Pro x64 |
| 编译器 | GCC 13.3 / Clang 18.1 | MSVC 19.41 |
| 标准 | C++20 (`-O3 -march=native`) | C++20 (`/O2`) |
| 后端 | epoll (默认) / io_uring | IOCP |
| ASIO | standalone 1.28.0 | standalone 1.28.0 |
| 工具 | redis_loadgen（统一压测，阻塞线程模型） | redis_loadgen |

### 6.2 epoll 后端性能

**单线程 — 编译器对比（10000 PING × 50 conn，redis_loadgen）**

| 服务器 | GCC 13.3 | 相对 ASIO | Clang 18.1 | 相对 ASIO |
|--------|-----:|:---:|-----:|:---:|
| coronet_ST | 25,856 | 55.1% | 28,336 | 87.9% |
| coronet_chain | 36,529 | 77.8% | 28,209 | 87.5% |
| **ASIO_ST** | **46,935** | — | **32,251** | — |

ASIO_ST 在 epoll 单线程下领先。回调模型在 readiness-based 后端下边际开销更低。Clang 下差距缩小（+12-14%），GCC 下 ASIO 优势更大。

### 6.3 io_uring 后端性能

coronet 使用原生 io_uring 后端（`IOSQE_IO_LINK` 内核链式 I/O），ASIO 使用 epoll reactor。

**单线程 — 编译器���比**

| 服务器 | GCC 13.3 | 相对 ASIO | Clang 18.1 | 相对 ASIO |
|--------|-----:|:---:|-----:|:---:|
| coronet_ST | 42,037 | 96.9% | **46,326** | **109.5%** |
| coronet_chain | 37,507 | 86.4% | 36,043 | 85.2% |
| ASIO_ST | 43,388 | — | 42,307 | — |

**多线程（6 线程共享 1 端口）— 编译器对比**

| 服务器 | GCC 13.3 | 相对 ASIO | Clang 18.1 | 相对 ASIO |
|--------|-----:|:---:|-----:|:---:|
| coronet_MT(6) | 137,375 | **116.0%** | **188,548** | **145.8%** |
| ASIO_MT(6) | 118,387 | — | 129,308 | — |

**epoll → io_uring 提升**

| 服务器 | GCC epoll | GCC io_uring | 提升 | Clang epoll | Clang io_uring | 提升 |
|--------|-----:|-----:|:---:|-----:|-----:|:---:|
| coronet_ST | 25,856 | 42,037 | **+63%** | 28,336 | 46,326 | **+64%** |
| coronet_MT | 134,885 | 137,375 | +2% | 101,063 | 188,548 | **+87%** |

coronet_ST 的 io_uring 提升在两个编译器上完全一致（+63% vs +64%）— 架构红利，非编译器优化。

### 6.4 Windows IOCP 性能

**CRTP 重构：虚函数 → 编译期多态**

将 `win_awaiter` 的纯虚函数 `issue_io()` 改为模板基类 `win_awaiter_base<Derived>`，消除 vtable 间接调用。

| 项目 | 旧（虚函数） | 新（CRTP） |
|------|:---------:|:---------:|
| 基类 | `class win_awaiter`（纯虚基类，vptr 8B） | `template<Derived> win_awaiter_base`（无 vtable） |
| issue_io 分派 | `this->issue_io()` → vtable | `static_cast<Derived*>(this)->issue_io()` → 直接调用 |
| 内联能力 | 不可内联 | 编译器可直接内联到 WSARecv/WSASend |

**CRTP 前后性能对比**

| 测试 | 重构前 | 重构后 | 变化 |
|------|:-----:|:-----:|:----:|
| coronet_ST | 47,945 RPS | **55,056 RPS** | **+14.8%** |
| coronet_chain | 56,029 RPS | **58,323 RPS** | **+4.1%** |
| coronet_MT(6) | 59,086 RPS | 59,317 RPS | +0.4% |

多线程下锁争用（mutex cross_queue）和 IOCP GQCS 内核瓶颈占主导，CRTP 优化被稀释。

**多线程（6 线程共享 1 端口）**

coronet 使用跨线程 co_spawn 分发，ASIO 使用 `io_context::run()` 多线程共享。

```
coronet_MT(6) — 跨线程 co_spawn          ASIO_MT(6) — 共享 io_context
────────────────────────────             ──────────────────────────
6 × io_context (独立线程，各 1 线程)      1 × io_context (6 线程共享)
worker[0]: acceptor::accept() + session  all threads: run() → GQCS
worker[1-5]: session only
accept → cross-thread co_spawn          IOCP 原生分发 completion
  │ mutex lock → push cross_queue         │ GQCS 唤醒任意线程
  └─ PostQueuedCompletionStatus           │
     → 目标 worker drain_cross_thread()   │
```

### 6.5 编译器性能对比

**全平台 MT 总览（6 线程共享 1 端口）**

| 编译器/后端 | coronet_MT | ASIO_MT | coronet/ASIO | 胜者 |
|:---|-----:|-----:|:---:|:---:|
| **Clang 18.1 + io_uring** | **188,548** | 129,308 | **145.8%** | **coronet +46%** |
| GCC 13.3 + io_uring | 137,375 | 118,387 | 116.0% | coronet +16% |
| GCC 13.3 + epoll | 134,885 | 137,405 | 98.2% | ASIO (+1.8%) |
| Clang 18.1 + epoll | 101,063 | 84,170 | 120.1% | coronet (+20%) |
| MSVC 19.41 + IOCP | 59,317 | 59,719 | 99.3% | 平手 |

全平台最高单次记录：Clang io_uring coronet_MT(6) = **188,548 RPS**。

**结论：**

- **后端选择 > 编译器选择 > 框架选择**
- io_uring 下 coronet 在所有编译器上均领先 ASIO（+16% ~ +46%）
- epoll 下编译器定胜负（GCC→ASIO 赢，Clang→coronet 赢）
- IOCP 下基本持平（CRTP 后 93%~99%）
- coronet_ST 的 io_uring 提升在 GCC/Clang 上完全一致（+63% vs +64%）— 架构红利，非编译器优化

**编译器差异分析：**

- **GCC**：对 ASIO 模板代码生成最优，epoll 下 ASIO 领先 82%
- **Clang**：ThinLTO 跨模块优化对 coronet（`.a` 静态库）收益显著
- **MSVC**：帧省略对协程优化最激进，绝对值受 IOCP 平台限制

### 6.6 链式 co_await

`co_await (send && recv)` — 单次挂起完成两个 I/O 操作。

| 平台 | 链式机制 | 开销 |
|------|:------:|:---:|
| io_uring | `IOSQE_IO_LINK`（内核级） | 零 userspace |
| epoll / IOCP | `chain_fn`（用户态回调） | 1 次间接调用 |

### 6.7 跨线程 co_spawn 架构

```
co_spawn_cross(handle)
  │ mutex lock → push cross_queue
  │ mutex unlock
  ├─ if (was_empty) proactor->wakeup()  ← 批量唤醒优化
  ▼
目标 worker:
  drain_cross_thread() → forward_task() → SPSC reap_swap → schedule()
```

批量唤醒优化（空→非空才调用 wakeup）减少 PQCS/eventfd 调用 90%+。

### 6.8 关键 Bug 修复

| Bug | 影响 | 修复 |
|------|------|------|
| SPSC `pop()` 未掩码 | 16384 次迭代后 SIGSEGV | `return h & mask` |
| reap_swap 栈溢出 (Win) | 10×io_context → 1.3MB 栈 | `array` → `vector`（堆） |
| io_uring timeout UAF | `__kernel_timespec` 栈变量 → 悬挂指针 | 改为成员变量 |
| `task_promise<void>` 析构 UB | union 未构造 `exception_ptr` → 双重释放 | `has_exception_` 标志 |
| `epoll_event.data` union 冲突 | `data.ptr` 覆盖 `data.fd` → EBADF | `epoll_completion_ctx` 独立存储 `fd` |
| tls_context 移动后 UAF | alpn_protocols_ 未转移 → SSL_CTX app_data 悬挂 | 转移 alpn_protocols_ |

### 6.9 构建与测试

```bash
# Linux / WSL — 构建全部 + 测试
cmake -B build -DCORONET_DEVELOPER_MODE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build && ctest --output-on-failure

# Windows MSVC — 构建全部 + 测试
cmake -B build -DCORONET_DEVELOPER_MODE=ON
cmake --build build --config Release
cd build && ctest -C Release --output-on-failure

# 切换后端
# epoll（默认）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
# io_uring
cmake -S . -B build -G Ninja -DCORONET_IOURING=ON

# 单独压测
./stress-test/stress_driver \
    --server "coronet_ST:redis_echo_ST:6380" \
    --server "ASIO_ST:redis_echo_asio_ST:6382" \
    -n 10000 -c 50
```

测试矩阵（2026-07-01）：

| 平台/编译器 | 后端 | 测试数 | 结果 |
|:---|:---|:---:|:---:|
| Linux GCC 13.3 | epoll | 23/23 | 通过 |
| Linux Clang 18.1 | epoll | 23/23 | 通过 |
| Windows MSVC 19.41 | IOCP | 22/22 | 通过 |

---

## API 速查表

| 分类 | 头文件 | 关键类型/函数 |
|------|--------|---------------------|
| **核心** | `coronet/task.hpp` | `task<T>`, `task<void>`, `task<T&>` |
|  | `coronet/io_context.hpp` | `io_context`, `co_spawn()`, `this_io_context()` |
|  | `coronet/shared_task.hpp` | `shared_task<T>` |
|  | `coronet/generator.hpp` | `generator<T>` |
| **I/O** | `coronet/async_io.hpp` | `async::recv/send/accept/connect/close/timeout/yield/read/write` |
| **网络** | `coronet/net.hpp` | `socket`, `acceptor`, `inet_address` |
| **TLS** | `coronet/net/tls.hpp` | `tls_context`, `tls_socket`, `tls_acceptor` |
| **同步** | `coronet/co/mutex.hpp` | `mutex`, `lock_guard` |
|  | `coronet/co/condition_variable.hpp` | `condition_variable` |
|  | `coronet/co/semaphore.hpp` | `counting_semaphore` |
|  | `coronet/co/channel.hpp` | `channel<T, N>` |
| **组合** | `coronet/co/when_all.hpp` | `all()`, `any()`, `some()`, `overload{}` |
| **工具** | `coronet/utility/defer.hpp` | `defer{}` |
| **All-in-one** | `coronet/all.hpp` | 包含以上全部 |
