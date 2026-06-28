# coronet

**跨平台高性能异步 I/O 库 · C++20 协程架构**

coronet 是一个基于 C++20 标准协程的异步 I/O 库，目标是在 **Linux**（io_uring）和 **Windows**（IOCP）上提供统一的协程化编程接口，让异步网络编程如同同步代码一样简单直观。

> 本项目参考 [co_context](https://github.com/Code-Deng/co_context) 的架构设计，在其基础上重构为跨平台实现。

---

## 目录

- [设计哲学](#设计哲学)
- [架构总览](#架构总览)
- [核心技术要点](#核心技术要点)
- [快速开始](#快速开始)
- [协程应用示例](#协程应用示例)
- [API 参考](#api-参考)
- [双平台支持](#双平台支持)
- [构建指南](#构建指南)
- [测试与基准](#测试与基准)
- [项目结构](#项目结构)

---

## 设计哲学

### 为什么是 Proactor？

I/O 模型分为两大类：

| 模型 | 代表 | 工作方式 | 协程友好度 |
|---|---|---|---|
| **Reactor** | epoll, kqueue | 通知"可读/可写"，用户负责读写 | ❌ 需手动管理缓冲区 |
| **Proactor** | IOCP, io_uring | 内核完成读写后通知结果 | ✅ `co_await` 直接获得结果 |

Linux **io_uring** 和 Windows **IOCP** 都是 Proactor 模式——操作系统内核异步完成 I/O 操作后通知用户程序。coronet 在此基础上抽象出一层统一的 Proactor 接口，向上层提供一致的协程体验。

### 两级调度

```
协程链（内联执行）          I/O 完成（调度器介入）
┌─────────────┐           ┌──────────────────┐
│ task A      │           │ io_uring / IOCP  │
│   co_await B│──内联──▶   │   完成事件到达    │
│   co_await C│──内联──▶   │   → 推入 reap    │
│   ...       │           │   → resume 协程   │
└─────────────┘           └──────────────────┘
```

- **内联路径**：`task<>` 链通过 `parent_coro` 直连，零调度开销
- **调度路径**：I/O 完成事件通过无锁 SPSC 环形队列（reap_swap）传递

---

## 架构总览

```
┌─────────────────────────────────────────────────────────┐
│                    用户代码 (User Code)                   │
│  task<> / shared_task<> / generator<>                   │
│  async::recv / async::send / ...                        │
│  mutex / condition_variable / semaphore / channel       │
│  socket / acceptor / inet_address                       │
└──────────────────────┬──────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────┐
│                   io_context (事件循环)                   │
│                                                         │
│  ┌──────────────────────────────────────────────────┐   │
│  │  worker_meta                                      │   │
│  │  ┌─────────────┐  ┌──────────────────────────┐   │   │
│  │  │  reap_swap   │  │  proactor*               │   │   │
│  │  │  (SPSC Ring) │  │  (平台抽象)              │   │   │
│  │  └─────────────┘  └───────────┬──────────────┘   │   │
│  └──────────────────────────────────────────────────┘   │
└──────────────────────┬──────────────────────────────────┘
                       │
          ┌────────────┴────────────┐
          ▼                         ▼
┌──────────────────┐    ┌──────────────────┐
│ io_uring_proactor│    │ iocp_proactor    │
│   (Linux)        │    │   (Windows)      │
│                  │    │                  │
│ • get_sq_entry() │    │ • GetQueued-     │
│ • submit_and_wait│    │   Completion-    │
│ • peek_cq_entry()│    │   Status         │
│ • cq_advance()   │    │ • PostQueued-    │
│                  │    │   Completion-    │
│                  │    │   Status         │
└──────────────────┘    └──────────────────┘
```

### 核心组件

| 组件 | 职责 |
|---|---|
| **`io_context`** | 事件循环调度器，每线程一个实例 |
| **`worker_meta`** | 管理 ready 队列 + I/O 提交/收割 |
| **`proactor`** | 平台抽象基类（io_uring / IOCP） |
| **`task<T>`** | 惰性协程任务，move-only，parent-chain |
| **`shared_task<T>`** | 引用计数多等待者 |
| **`lazy_*` awaiters** | 具体 I/O awaitable（recv/send/accept/...） |

---

## 核心技术要点

### 1. 惰性执行 (Lazy Execution)

`task<T>` 在 `initial_suspend()` 返回 `suspend_always`，协程体不会自动开始执行。只有被 `co_await` 时才会运行：

```cpp
task<int> compute() { co_return 42; }

task<void> example() {
    auto t = compute();     // ❌ 尚未开始执行
    int v = co_await t;     // ✅ 开始执行并等待结果
}
```

### 2. 零开销父链 (Zero-Overhead Parent Chain)

`task_final_awaiter::await_suspend()` 直接返回父协程句柄，控制权同步转移，无需调度器介入：

```cpp
// 编译器展开后的流程：
child 完成 → final_suspend → return parent_handle → 恢复父协程
// 全程无调度器、无锁、无原子操作
```

### 3. 无锁 SPSC 环形队列

`reap_swap` 是一个单生产者单消费者的无锁环形缓冲区，用于从 I/O 完成事件到协程恢复的传递：

```
生产者（I/O 完成）： push(handle) → tail++
消费者（事件循环）： pop() → head++
当 head == tail 时，队列为空
```

### 4. 平台抽象 Proactor（静态多态）

```cpp
// C++20 concept — 编译期类型检查，零虚函数开销
template<typename T>
concept proactor_concept = requires(T p, completion_info* info) {
    { p.init(entries) } -> std::same_as<void>;
    { p.submit(false) } -> std::same_as<int>;
    { p.wait_completion(info) } -> std::same_as<int>;
    { p.wakeup() } -> std::same_as<void>;
};
```

- **Linux**: `io_uring_proactor` — 直接操作 io_uring SQ/CQ 环形缓冲区，eventfd 唤醒
- **Windows**: `iocp_proactor` — 封装 IOCP (CreateIoCompletionPort, GetQueuedCompletionStatus)，per-thread operation recycling

### 5. 平台无关 I/O 工厂

```cpp
namespace coronet::async {
    auto recv(int fd, std::span<char> buf, int flags = 0) noexcept;
    auto send(int fd, std::span<const char> buf, int flags = 0) noexcept;
    auto accept(int fd, sockaddr* addr, socklen_t* addrlen, int flags) noexcept;
    auto connect(int fd, const sockaddr* addr, socklen_t addrlen) noexcept;
    auto close(int fd) noexcept;
    auto yield() noexcept;
    auto timeout(auto dur) noexcept;
}
```

每个工厂函数在编译期根据平台宏分派到具体实现：
- Linux → `detail::io_uring_recv{fd, buf, flags}`（直接填充 SQE）
- Windows → `detail::win_recv{(uintptr_t)fd, buf, flags}`（记录参数，await_suspend 时发起 WSARecv）

---

## 快速开始

### 依赖

- C++20 编译器（GCC 13+, MSVC 19.41+, Clang 14+）
- CMake 3.20+
- Linux: 内核 5.19+（推荐 6.1+）
- Windows: Windows 8+（IOCP）

### 构建

```bash
# Linux
git clone https://github.com/your/coronet.git
cd coronet
mkdir build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
ctest --output-on-failure

# Windows (需先执行 vcvars64.bat)
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
ctest --output-on-failure
```

### CMake 选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `CORONET_BUILD_TESTS` | ON | 构建 gtest 单元测试 |
| `CORONET_BUILD_BENCHMARKS` | OFF | 构建 Google Benchmark |
| `CORONET_BUILD_EXAMPLES` | ON | 构建示例程序 |
| `CORONET_USE_MIMALLOC` | OFF | 使用 mimalloc 优化分配 |

---

## 协程应用示例

### Echo Server

```cpp
#include <coronet/coronet.hpp>
using namespace coronet;

// 单个连接的处理协程
task<> session(int fd) {
    char buf[1024];
    while (true) {
        int n = co_await async::recv(fd, buf);
        if (n <= 0) break;           // 连接关闭或错误
        co_await async::send(fd, {buf, (size_t)n});  // 回显
    }
    co_await async::close(fd);
}

// 主监听协程
task<> server(uint16_t port) {
    acceptor ac{inet_address{port}};
    while (true) {
        int fd = co_await ac.accept();           // await 新连接
        co_spawn(session(fd));                   // spawn 处理协程
    }
}

int main() {
    io_context ctx;
    ctx.co_spawn(server(8080));
    ctx.start();
    ctx.join();    // 永不返回（除非收到停止信号）
}
```

### 多线程并发

```cpp
// 创建多个 io_context，每个运行在自己的线程上
std::vector<io_context> ctxs(4);
for (auto& ctx : ctxs) ctx.start();

// 通过 shared_task 在多个 context 间共享结果
shared_task<int> shared = compute();

// 可在任意线程 co_await 同一个 shared_task
for (auto& ctx : ctxs) {
    ctx.co_spawn(worker(shared));
}
for (auto& ctx : ctxs) ctx.join();
```

### 定时器

```cpp
task<> tick() {
    while (true) {
        printf("tick\n");
        co_await async::timeout(std::chrono::seconds(1));
    }
}
```

### 同步原语

```cpp
// 使用 mutex 保护共享资源
mutex mtx;
int shared_counter = 0;

task<> increment() {
    auto guard = co_await mtx.lock_guard();  // RAII 锁
    ++shared_counter;
}   // 自动释放

// 使用 channel 在协程间通信
channel<int, 64> ch;

task<> producer() {
    for (int i = 0; i < 100; ++i)
        co_await ch.release(i);
}

task<> consumer() {
    for (int i = 0; i < 100; ++i) {
        int v = co_await ch.acquire();
        printf("got %d\n", v);
    }
}
```

---

## API 参考

完整 API 手册（14 节，含代码示例）→ **[doc/api_Manual.md](doc/api_Manual.md)**

| 类别 | 关键 API |
|------|---------|
| **核心** | `task<T>`, `io_context`, `shared_task<T>`, `generator<T>` |
| **I/O** | `async::recv/send/accept/connect/close/timeout/yield/read/write` |
| **网络** | `socket`, `acceptor`, `inet_address` |
| **同步** | `mutex`, `condition_variable`, `counting_semaphore`, `channel<T,N>` |
| **组合** | `all()`, `any()`, `some()`, `defer{}` |

---

## 双平台支持

| 特性 | Linux | Windows |
|---|---|---|
| 异步 I/O 后端 | io_uring | IOCP |
| Proactor 实现 | `io_uring_proactor` | `iocp_proactor` |
| 事件提交 | `io_uring_enter()` / `submit_and_wait()` | `PostQueuedCompletionStatus()` |
| 事件收割 | `peek_cq_entry()` / `cq_advance()` | `GetQueuedCompletionStatus()` |
| socket 创建 | `socket()` + SOCK_CLOEXEC | `WSASocketW()` + WSA_FLAG_OVERLAPPED |
| socket I/O | `prep_recv()` / `prep_send()` | `WSARecv()` / `WSASend()` |
| close | `close()` | `closesocket()` |
| 地址族 | `sa_family_t` | `u_short` |
| 地址长度 | `socklen_t` | `int` |

---

## 构建指南

### Linux (WSL / 原生)

```bash
# 依赖：g++-13, cmake, ninja
sudo apt install g++-13 cmake ninja-build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### Windows (Visual Studio 2022)

```powershell
# 从 Visual Studio Developer Command Prompt (x64) 运行
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

---

## 三平台性能 (Redis PING 服务, 100K 请求)

### 单线程

| c | coronet GCC | co_context GCC | ASIO GCC | coronet Clang | coronet MSVC | ASIO MSVC |
|:--:|:---:|:---:|:---:|:---:|:---:|:---:|
| 10 | 37,764 | 42,680 | **43,630** | 40,800 | **53,361** | 54,957 |
| 50 | 43,197 | **49,850** | 47,192 | **47,304** | **58,384** 🏆 | 47,045 |
| 200 | 38,197 | 43,011 | **53,591** | 41,085 | **46,463** | 44,876 |
| 500 | **48,662** | 42,517 | 42,391 | 41,754 | **44,210** | 44,134 |

### 多线程 (6线程)

| c | coronet GCC | ASIO GCC | coronet Clang | ASIO Clang | coronet MSVC | ASIO MSVC |
|:--:|:---:|:---:|:---:|:---:|:---:|:---:|
| 10 | 18,005 | **29,700** | 25,439 | **30,303** | **56,101** | 52,961 |
| 100 | 35,651 | **37,750** | 33,979 | 25,681 | **51,571** | 49,545 |
| 500 | 32,992 | **34,626** | **38,226** | 30,039 | **44,115** | 41,062 |

**三平台 70 次压测零崩溃**。完整报告：[doc/aio_PR.md](doc/aio_PR.md)

---

## 项目结构

```
coronet/
├── include/coronet/           # 公共 API 头文件
│   ├── task.hpp               # 惰性协程任务
│   ├── generator.hpp          # P2502R2 生成器
│   ├── shared_task.hpp        # 引用计数多等待者
│   ├── io_context.hpp         # 事件循环调度器
│   ├── async_io.hpp           # 跨平台 I/O 工厂
│   ├── net/                   # socket/acceptor/inet_address
│   ├── co/                    # mutex/CV/semaphore/channel
│   ├── platform/              # proactor 抽象 + 平台实现
│   └── detail/                # 内部实现细节
├── lib/coronet/               # 编译后实现 (.cpp)
├── extern/                    # 第三方依赖
│   ├── liburingcxx/           # io_uring C++ wrapper
│   ├── googletest/            # 单元测试框架
│   └── benchmark/             # 性能基准框架
├── test/                      # 单元测试
├── bench/                     # 性能基准
├── examples/                  # 示例程序
├── script/                    # 构建脚本
└── doc/                       # 文档
```

---

## License

MIT License

## Acknowledgments

- [co_context](https://github.com/Codesire-Deng/co_context) — Architecture reference
- [liburingcxx](https://github.com/Codesire-Deng/co_context/tree/master/extern/liburingcxx) — io_uring C++ wrapper
- [ASIO](https://think-async.com/Asio/) — IOCP patterns reference
- [P2502R2](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p2502r2.pdf) — `std::generator` reference implementation
