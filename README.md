# coronet

**C++20 协程 · 跨平台 · 高性能异步 I/O 库**

<p align="center">
  <img src="https://img.shields.io/badge/C++-20-blue" alt="C++20">
  <img src="https://img.shields.io/badge/Linux-GCC%2013%20%7C%20Clang%2018-brightgreen" alt="Linux">
  <img src="https://img.shields.io/badge/Windows-MSVC%2019.41-blue" alt="Windows">
  <img src="https://img.shields.io/badge/I/O-epoll%20%7C%20io__uring%20%7C%20IOCP-orange" alt="IO">
  <img src="https://img.shields.io/badge/test-22%2F22%20passed-success" alt="Tests">
</p>

---

## ✨ 核心亮点

| 🎯 | 特性 | 说明 |
|:--:|------|------|
| 🔌 | **三后端热插拔** | epoll（默认）⇄ io_uring（`-DCORONET_IOURING=ON`）⇄ IOCP（Windows 自动） |
| 🖥️ | **全编译器跨平台** | Linux GCC 13 / Clang 18 + Windows MSVC 19.41，一套代码 |
| ⚡ | **编译期零开销多态** | CRTP + `#ifdef` 平台选择，无虚表、无堆分配 Proactor |
| 🔗 | **链式 co_await** | `co_await (recv && send)` 单次挂起完成两个 I/O 操作 |
| 🧵 | **协程同步原语** | mutex / condition_variable / semaphore / channel / when_all·any·some |
| 📊 | **统一压测驱动** | `stress_driver --server name:binary:port` 自动采集 RPS + CPU + 内存 |

## 🚀 性能速览

**Redis PING 服务, 100K 请求 × 50 并发, WSL2 epoll**

| 服务端 | RPS | CPU% | 内存 |
|--------|-----:|:---:|-----:|
| coronet ST (协程) | 19,952 | 83.6% | 3.9 MB |
| coronet chain (链式) | 14,051 | 87.1% | 3.9 MB |
| ASIO ST (回调) | 19,175 | 73.2% | 3.7 MB |

**三平台峰值 RPS（单线程）**

| 编译器 | coronet | 后端 |
|--------|-----:|------|
| MSVC 19.41 (Windows) | **58,384** 🏆 | IOCP |
| GCC 13.3 (Linux) | 48,662 | io_uring |
| Clang 18.1 (Linux) | 47,304 | io_uring |

> 完整报告 → [doc/aio_PR.md](doc/aio_PR.md) | 测试报告 → [doc/test_Report.md](doc/test_Report.md)

---

## ⚡ 快速开始

```bash
# 克隆
git clone https://github.com/lsqyling/coronet.git && cd coronet

# Linux — 默认 epoll 后端(Release)
cmake -S . -B build -G Ninja
    
cmake --build build
cd build && ctest --output-on-failure

# 切换到 io_uring
cmake -S . -B build-uring -G Ninja -DCORONET_IOURING=ON
cmake --build build-uring
ctest --test-dir build-uring --output-on-failure

# Windows MSVC (Developer Command Prompt)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

# Install coronet
cmake --install build --prefix D:/dev/local # windows msvc 
cmake --install build --prefix /usr/local    # linux 

# how to use
find_package(coronet REQUIRED)
add_executable(your_timer your_timer.cpp)
target_link_libraries(your_timer PRIVATE coronet::coronet)
```

## 📖 30 秒示例

### Echo Server

```cpp
#include <coronet/coronet.hpp>
using namespace coronet;

task<> session(int fd) {
    char buf[1024];
    while (true) {
        int n = co_await async::recv(fd, buf);
        if (n <= 0) break;
        co_await async::send(fd, {buf, (size_t)n});
    }
}

task<> server(uint16_t port) {
    acceptor ac{inet_address{port}};
    while (true)
        co_spawn(session(co_await ac.accept()));
}

int main() {
    io_context ctx;
    ctx.co_spawn(server(8080));
    ctx.start(); ctx.join();
}
```

### 同步原语

```cpp
// 互斥锁
mutex mtx;
task<> critical() {
    auto g = co_await mtx.lock_guard();
    /* 临界区 */
}

// 信号量 — 10 协程竞争 3 槽位
counting_semaphore sem{3};
task<> worker() { co_await sem.acquire(); /* ... */ sem.release(); }

// 条件变量
condition_variable cv; mutex m;
task<> waiter() {
    auto lk = co_await m.lock_guard();
    co_await cv.wait(m, [] { return ready; });
}

// CSP 通道 — 生产者/消费者
channel<std::string, 4> ch;
task<> producer() { co_await ch.release("msg"); }
task<> consumer() { auto s = co_await ch.acquire(); }
```

### 协程组合器 + 链式 I/O

```cpp
// 等待全部完成
auto [r0, r1] = co_await all(taskA(), taskB(), taskC());

// 首个完成者胜出
auto [idx, var] = co_await any(taskA(), taskB());

// 链式 co_await — 发送 PONG 同时接收下一条 PING
co_await (async::send(fd, pong) && async::recv(fd, buf));
```

---

## 🏗️ 架构

```
用户代码: task<> / shared_task<> / generator<>
   ↓ co_await
async::recv / send / accept / connect / timeout / ...
   ↓ 工厂函数 (编译期分派)
┌──────────┬──────────────┬──────────┐
│  epoll   │   io_uring   │   IOCP   │  ← 三后端, 编译期选择
│ (默认)   │ (CORONET_   │ (Windows │
│          │   IOURING=ON)│  自动)   │
└──────────┴──────────────┴──────────┘
   ↓
io_context (单线程事件循环)
   ├─ drain_cross_thread()   跨线程队列 → SPSC 环
   ├─ do_worker_part()       SPSC 环 → resume 协程
   ├─ do_submission_part()   提交 I/O (仅 io_uring)
   └─ do_completion_part()   收割完成事件
```

| 组件 | 职责 |
|------|------|
| `io_context` | 单线程事件循环，栈上 Proactor |
| `worker_meta` | SPSC 无锁环 + 跨线程队列 + I/O 计数器 |
| `task<T>` | 惰性协程，父链内联恢复（零调度开销） |
| `shared_task<T>` | 引用计数多等待者 |
| `epoll_awaiter_base<D>` | CRTP 编译期多态（非虚函数） |

---

## 📊 调用流程 / Call-Flow Diagrams

下面通过 Mermaid 时序图和数据流图展示 coronet 核心机制的运行时调用关系。

### 1. Fibonacci 生成器 — 协程生命周期

`test/generator_gtest.cpp` 中 Fibonacci 测试用例的完整调用时序。

```mermaid
sequenceDiagram
    participant Test
    participant Fib as "fib() coroutine"
    participant Promise as "promise_type"
    participant Iterator as "_Gen_iter"
    participant Awaiter as "_Element_awaiter"
    participant Final as "_Final_awaiter"

    Test->>Fib: fib() called
    activate Fib
    Note over Fib: 编译器分配协程帧
    Fib->>Promise: promise_type::operator new
    Fib->>Promise: initial_suspend() returns suspend_always
    Fib->>Test: return generator handle
    deactivate Fib
    Note over Fib: 协程体尚未执行 - 惰性求值

    rect rgb(240, 248, 255)
    Note over Test,Iterator: 范围 for: begin()
    Test->>Iterator: begin() calls _Coro.resume()
    activate Iterator
    Iterator->>Fib: 首次恢复执行
    activate Fib
    Note over Fib: a=0, b=1, while true
    Fib->>Awaiter: co_yield a -> yield_value(a)
    activate Awaiter
    Note over Awaiter: 拷贝值, 存入 _Ptr
    Awaiter->>Fib: await_suspend - 协程挂起
    deactivate Awaiter
    deactivate Fib
    Iterator->>Test: return iterator handle
    deactivate Iterator

    Note over Test,Iterator: 第1次迭代: value = 0
    Test->>Iterator: *it (operator*)
    Iterator->>Iterator: _Coro.promise()._Top.promise()._Ptr
    Iterator->>Test: 0
    Test->>Test: results.push_back(0)

    Test->>Iterator: ++it (operator++)
    activate Iterator
    Iterator->>Fib: _Top.resume()
    activate Fib
    Note over Fib: next=1, a=1, b=1
    Fib->>Awaiter: co_yield 1 -> yield_value(1)
    activate Awaiter
    Awaiter->>Fib: await_suspend - 挂起
    deactivate Awaiter
    deactivate Fib
    deactivate Iterator

    Note over Test,Iterator: 重复 11 次...
    Note over Test,Iterator: 最后一次: value = 89

    Test->>Iterator: ++it (最后一次)
    activate Iterator
    Iterator->>Fib: _Top.resume()
    activate Fib
    Note over Fib: a=144 > 100, 循环退出
    Fib->>Promise: return_void()
    Promise->>Final: final_suspend()
    activate Final
    Note over Final: _Info == nullptr (无嵌套)
    Final->>Final: return noop_coroutine()
    deactivate Final
    Note over Fib: done() == true
    deactivate Fib
    deactivate Iterator

    Test->>Iterator: it == end -> 循环退出
    Note over Test: range-for 结束
    end

    Test->>Fib: ~generator() 析构
    activate Fib
    Fib->>Promise: _Coro.destroy()
    Note over Promise: 释放协程帧
    deactivate Fib
```

---

### 2. 异步定时器 — 3 个并发定时器

`test/timer.cpp` 中启动 3 个定时器的测试。两个 1 秒定时器各运行 2 轮，一个 3 秒定时器运行 1 轮，外加 6 秒停止协程。

```mermaid  
sequenceDiagram
    participant Main
    participant Ctx as io_context
    participant EvLoop as EventLoop
    participant T1 as "cycle_n (1s, 2rds)"
    participant T1b as "cycle_n (1s, 2rds) rel"
    participant T3 as "cycle_n (3s, 1rd)"
    participant Stop as "stop_after (6s)"
    participant Timer as "platform::timeout"

    Main->>Ctx: co_spawn(task) x4
    Note over Ctx: 全部协程在 initial_suspend 挂起
    Main->>Ctx: start() (启动事件循环线程)

    activate EvLoop
    EvLoop->>EvLoop: drain_cross_thread()
    EvLoop->>EvLoop: do_worker_part()

    par 定时器并行启动
        T1->>T1: co_await async::timeout(1s)
        T1->>Timer: 注册定时器
        T1->>T1: suspend
        T1b->>T1b: co_await async::timeout(1s)
        T1b->>Timer: 注册定时器
        T1b->>T1b: suspend
        T3->>T3: co_await async::timeout(3s)
        T3->>Timer: 注册定时器
        T3->>T3: suspend
        Stop->>Stop: co_await async::timeout(6s)
        Stop->>Timer: 注册定时器
        Stop->>Stop: suspend
    end

    Note over Timer: ~1 秒后
    Timer->>EvLoop: 两个 1s 定时器到期
    EvLoop->>EvLoop: handle_completion -> forward_task
    EvLoop->>T1: resume (第1轮完成)
    EvLoop->>T1b: resume (第1轮完成)
    T1->>Timer: 第2轮 co_await async::timeout(1s)
    T1b->>Timer: 第2轮 co_await async::timeout(1s)

    Note over Timer: ~2 秒后
    Timer->>EvLoop: 两个 1s 定时器再次到期
    EvLoop->>T1: resume (第2轮完成, 退出)
    EvLoop->>T1b: resume (第2轮完成, 退出)

    Note over Timer: ~3 秒后
    Timer->>EvLoop: 3s 定时器到期
    EvLoop->>T3: resume (完成, 退出)

    Note over Timer: ~6 秒后
    Timer->>EvLoop: 6s 定时器到期
    EvLoop->>Stop: resume
    Stop->>Ctx: can_stop()
    Ctx->>EvLoop: will_stop_ = true
    deactivate EvLoop

    Main->>Ctx: join() 返回, 测试结束
```

```mermaid  
flowchart TD
    A["cycle_n 协程<br/>co_await async::timeout(D)"] --> B["make_timeout(dur)<br/>async_io.hpp 工厂函数"]
    B --> C["platform_io::make_timeout(dur)"]
    C --> D{"编译期平台选择<br/>Compile-time dispatch"}

    D -->|Windows IOCP| E1["win_timeout::issue_io()<br/>iocp_win_io.hpp:408"]
    E1 --> F1["后台线程 / Background thread<br/>Sleep(ms)"]
    F1 --> G1["on_sync_completion()<br/>PostQueuedCompletionStatus"]

    D -->|Linux io_uring| E2["io_uring_timeout<br/>prep_timeout(SQE)"]
    E2 --> F2["do_submission_part()<br/>io_uring_enter 提交"]
    F2 --> G2["内核 CQE ready<br/>IORING_OP_TIMEOUT"]

    D -->|Linux epoll| E3["epoll_timeout<br/>timerfd_settime()"]
    E3 --> F3["epoll_wait 返回"]
    F3 --> G3["do_perform → read timerfd"]

    G1 --> H["proactor.wait_completion()<br/>收割完成事件"]
    G2 --> H
    G3 --> H

    H --> I["worker_meta::handle_completion()<br/>解码 task_info, 设置 result"]
    I --> J["forward_task(handle)<br/>→ SPSC 环 (lock-free)"]
    J --> K["do_worker_part()<br/>从 SPSC 环弹出"]
    K --> L["coroutine_handle::resume()"]
    L --> M["co_await async::timeout() 返回"]
    M --> A
```

---

### 3. CSP 通道 — 3 生产者 / 3 消费者

`test/channel.cpp` 测试：3 个生产者各发送 4 条消息（2 条快速 + 2 条带 200ms 延迟），共 12 条消息被 3 个消费者并发消费。

```mermaid
sequenceDiagram
    participant Main
    participant Ctx as io_context
    participant EvLoop as EventLoop
    participant P0 as "produce(p0)"
    participant P1 as "produce(p1)"
    participant P2 as "produce(p2)"
    participant C0 as "consume(c0)"
    participant C1 as "consume(c1)"
    participant C2 as "consume(c2)"
    participant Ch as "channel (buffer=4)"
    participant Stop as "stopper (8s)"

    Main->>Ctx: co_spawn 7 tasks (3P + 3C + stopper)
    Main->>Ctx: start()
    activate EvLoop
    EvLoop->>EvLoop: drain_cross_thread + do_worker_part

    par 快速生产阶段
        P0->>Ch: release("p0: fast produce") 第1次
        Note over Ch: lock, !full(), construct_at, push_one<br/>unlock, notify not_empty
        P0->>Ch: release("p0: fast produce") 第2次
        P1->>Ch: release("p1: fast produce") x2
        P2->>Ch: release("p2: fast produce") x2
    end

    Note over Ch: 缓冲占用: 6/4 (部分生产者等待 not_full)

    par 消费阶段
        C0->>Ch: acquire()
        Note over Ch: lock, !empty(), move item<br/>destroy_at, pop_one, unlock
        Ch->>C0: return string
        C0->>C0: printf (1/12)
        C1->>Ch: acquire()
        Ch->>C1: return string
        C1->>C1: printf (2/12)
        C2->>Ch: acquire()
        Ch->>C2: return string
        C2->>C2: printf (3/12)
    end

    par 慢速生产 + 消费交替
        P0->>P0: async::timeout(200ms)
        P0->>Ch: release("p0: slow produce")
        C0->>Ch: acquire()
        Ch->>C0: return string
        Note over C0,C2: ... 共 12 条消息全部消费 ...
    end

    Note over C0,C2: 全部 12 条消息消费完毕

    Note over Stop: 安全兜底: 8 秒后强制停止
    Stop->>Ctx: can_stop()
    Ctx->>EvLoop: will_stop_ = true
    deactivate EvLoop
    Main->>Ctx: join() 返回
    Note over Main: assert(msg_consumed >= 12)
```

---

### 4. TCP Echo 服务器/客户端 — 双线程 Echo

`examples/echo_server_client.cpp` 中服务器端和客户端的完整调用时序和数据流。

```mermaid
sequenceDiagram
    participant Main
    participant SCtx as server_ctx
    participant Svr as echo_server
    participant Ses as echo_session
    participant CCtx as client_ctx
    participant Cli as echo_client
    participant SkC as socket(client)
    participant SkS as socket(server)
    participant TCP as TCP Kernel

    Note over Main: === 阶段 1: 启动服务器 ===
    Main->>SCtx: co_spawn(echo_server())
    SCtx->>Svr: resume
    activate Svr
    Note over Svr: acceptor(inet_address{9090})<br/>create_tcp → set_reuse → bind → listen
    Svr->>SkS: co_await ac.accept()
    Note over Svr: 挂起, 等待连接 / suspended, waiting for connection
    deactivate Svr

    Note over Main: sleep(100ms) — 给服务器时间启动

    Note over Main: === 阶段 2: 客户端连接 ===
    Main->>CCtx: co_spawn(echo_client(...))
    CCtx->>Cli: resume
    activate Cli
    Cli->>Cli: inet_address::resolve("127.0.0.1", 9090)
    Cli->>SkC: socket::create_tcp()
    Cli->>SkC: co_await sock.connect(addr)
    SkC->>TCP: SYN
    TCP->>SkS: 连接到达, accept 被唤醒
    activate Svr
    SkS->>Svr: accept 返回新连接 fd
    deactivate Svr
    Svr->>Ses: co_spawn(echo_session(sock))
    activate Ses
    SkC->>Cli: connect 返回 0 (连接成功)
    deactivate Cli

    Note over Main: === 阶段 3: 数据收发 (Echo) ===
    Cli->>SkC: co_await sock.send(TestMsg)
    SkC->>TCP: TCP 数据包
    TCP->>SkS: 数据到达
    Ses->>SkS: co_await sock.recv(buf)
    SkS->>Ses: recv 返回 nr 字节
    Ses->>SkS: co_await sock.send(buf, nr)
    SkS->>TCP: Echo 数据
    TCP->>SkC: Echo 返回
    Cli->>SkC: co_await sock.recv(buf)
    SkC->>Cli: recv 返回, 验证 Echo 内容

    Note over Main: === 阶段 4: 清理 ===
    Cli->>SkC: co_await sock.shutdown_write()
    Cli->>CCtx: ctx.can_stop()
    CCtx->>Main: join() 返回
    Main->>SCtx: ctx.can_stop()
    SCtx->>Main: join() 返回
    deactivate Ses
```

```mermaid
flowchart LR
    subgraph Main_Thread["主线程 / Main Thread (client_ctx)"]
        CL[echo_client 协程]
        SKC[socket client<br/>RAII fd]
    end

    subgraph Server_Thread["后台线程 / Server Thread (server_ctx)"]
        SV[echo_server 协程]
        AC[acceptor<br/>listen socket]
        SS[echo_session 协程]
        SKS[socket server<br/>RAII fd]
    end

    subgraph Kernel["操作系统内核 / Kernel"]
        TCP["TCP/IP 协议栈<br/>(loopback/Linux/Windows)"]
    end

    CL -->|"① connect(127.0.0.1:9090)"| SKC
    SKC -->|"SYN →"| TCP
    TCP -->|"accept 唤醒"| AC
    AC -->|"返回 connected fd"| SV
    SV -->|"co_spawn"| SS

    CL -->|"② send(TestMsg)"| SKC
    SKC -->|"TCP data →"| TCP
    TCP -->|"→ data in"| SKS
    SKS -->|"③ recv(buf)"| SS

    SS -->|"④ send({buf, nr})"| SKS
    SKS -->|"TCP echo →"| TCP
    TCP -->|"→ echo data"| SKC
    SKC -->|"⑤ recv(buf)"| CL

    CL -->|"⑥ 验证 echo 内容"| CL
    CL -->|"⑦ shutdown_write + can_stop()"| CL

    style CL fill:#d4f0d4,stroke:#333
    style SS fill:#d4f0d4,stroke:#333
    style AC fill:#e8e8ff,stroke:#333
    style TCP fill:#fff3cd,stroke:#333
    style SKC fill:#f0f0f0,stroke:#999
    style SKS fill:#f0f0f0,stroke:#999
```

---

## 📦 CMake 选项

| 选项 | 默认 | 说明 |
|------|:---:|------|
| `CORONET_IOURING` | OFF | 启用 io_uring 替代 epoll |
| `CORONET_BUILD_TESTS` | ON* | 21 项单元+集成测试 (CTest) |
| `CORONET_BUILD_BENCHMARKS` | ON | Google Benchmark 微基准 |
| `CORONET_BUILD_STRESS_TESTS` | OFF | 压力测试 (redis-benchmark + redis_loadgen) |
| `CORONET_BUILD_EXAMPLES` | ON* | 示例程序 |

> \* `PROJECT_IS_TOP_LEVEL` 时默认 ON

---

## 🧪 CTest 测试矩阵

```bash
# 运行全部
ctest --output-on-failure -j4

# 分类运行
ctest -R gtest          # 单元测试 (18 用例)
ctest -R benchmark      # Google Benchmark
ctest -R stress_driver  # 压测 (ST / MT)
```

| 平台 / 编译器 | 后端 | 测试数 | 结果 |
|:---|:---|:---:|:---:|
| Linux GCC 13.3 | epoll | 22/22 | ✅ |
| Linux Clang 18.1 | epoll | 22/22 | ✅ |
| Linux GCC 13.3 | io_uring | 22/22 | ✅ |
| Windows MSVC 19.41 | IOCP | 21/21 | ✅ |

---

## 🔬 压力测试

```bash
# 构建
cmake -S . -B build -DCORONET_BUILD_STRESS_TESTS=ON
cmake --build build

# CTest 单线程对比
ctest -R stress_driver_ST

# 手动 — 自定义服务端
./stress_driver \
  --server "coronet_ST:redis_echo_ST:6380" \
  --server "ASIO_ST:redis_echo_asio_ST:6382" \
  -n 100000 -c 100 -v
```

添加新服务端**无需改 stress_driver 代码** — 在 CMakeLists 中追加 `--server name:binary:port` 即可。

---

## 📂 目录

```
coronet/
├── include/coronet/       # 公共头文件
│   ├── task.hpp           #   惰性协程
│   ├── async_io.hpp       #   跨平台 I/O 工厂
│   ├── io_context.hpp     #   事件循环
│   ├── net/               #   socket / acceptor
│   ├── co/                #   mutex / cv / sem / channel
│   ├── platform/          #   epoll / io_uring / IOCP
│   └── detail/            #   内部实现
├── src/coronet/           # .cpp 实现
├── test/                  # 21 项 CTest
├── bench/                 # Google Benchmark
├── stress-test/           # 压测驱动 + 服务端
├── examples/              # 示例程序
├── doc/                   # 性能报告 / API 手册
└── cmake/                 # CMake 模块
```

---

## 📜 许可

[Apache License](./LICENSE)
---

<sub>本项目代码由 **Claude Code** (Deepseek) 辅助生成，采用 AI Vibe Coding 开发方式。人工进行需求定义、架构设计审核、代码审查及测试验证。</sub>
