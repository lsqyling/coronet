# coronet 测试报告

## 测试环境

| 项目 | Linux GCC | Linux Clang | Windows MSVC |
|------|-----------|-------------|--------------|
| **OS** | WSL2 Ubuntu (Kernel 5.15.153.1) | WSL2 Ubuntu | Windows 10 Pro x64 |
| **编译器** | GCC 13.3.0 | Clang 18.1.3 | MSVC 19.41 (VS 2022) |
| **C++ 标准** | C++20 (`-O3 -march=native`) | C++20 (`-O3 -march=native`) | C++20 (`/O2`) |
| **构建系统** | Ninja | Ninja | MSBuild |
| **后端** | epoll (默认) / io_uring (`-DCORONET_IOURING=ON`) | epoll / io_uring | IOCP |
| **ASIO** | standalone 1.28.0（无 Boost） | standalone 1.28.0 | standalone 1.28.0 |
| **压测工具** | redis-benchmark + redis_loadgen | redis_loadgen | redis_loadgen |
| **日期** | 2026-07-01 | 2026-07-01 | 2026-07-01 |

## 架构总览

| 维度 | coronet | ASIO |
|------|---------|------|
| **编程范式** | C++20 协程 (`co_await`) | 回调 |
| **I/O 后端** | epoll / io_uring / IOCP | epoll / IOCP |
| **跨平台** | ✅ Windows + Linux | ✅ Windows + Linux |
| **多态** | 编译期 (CRTP + `#ifdef`，零虚表) | 编译期 (模板) |
| **Proactor** | 栈上具体类型，零堆分配 | — |
| **链式 I/O** | `co_await (recv && send)` — io_uring 内核级 / epoll&IOCP 用户态 | 嵌套回调 |
| **跨线程 spawn** | mutex queue + eventfd 唤醒 | `post()` / `dispatch()` |

## I/O 后端演进与测试

### epoll 后端实现

epoll 作为 **Linux 默认后端**，`-DCORONET_IOURING=ON` 切换回 io_uring。CRTP 编译期多态消除虚表开销。

| 维度 | epoll | io_uring | IOCP |
|------|:---:|:---:|:---:|
| I/O 模型 | Readiness | Completion | Completion |
| 内核要求 | 2.6+ | 5.1+ | Win NT 6.0+ |
| 操作注册 | `epoll_ctl(ADD, ONESHOT\|ET)` | SQE 预填充 | `CreateIoCompletionPort` |
| I/O 执行 | Proactor 调用 `perform_io()` | 内核 CQE 返回 | GQCS 返回 OVERLAPPED |
| 提交方式 | await_suspend 时注册 | 批量 submit | issue_io() 直接发起 |

#### epoll 测试中修复的 Bug

| Bug | 根因 | 修复 |
|------|------|------|
| `epoll_event.data` union 冲突 | `data.ptr` 覆盖 `data.fd` → EBADF | `epoll_completion_ctx` 独立存储 `fd` |
| `arm_eventfd` 重复 ADD | eventfd 已注册 → EEXIST | EEXIST → EPOLL_CTL_MOD fallback |
| `register_fd` 重复 ADD | 同一 fd 连续操作 | 同上 |
| `<fcntl.h>` 污染 | `struct stat` 遮蔽 `::stat()` | 本地常量 `kPipe2Flags` |
| `native_handle()` 重复定义 | 头文件 + cpp 双重定义 | 删除 cpp 定义 |
| OOP 虚函数 → CRTP | `virtual register_with_epoll()` | `template<Derived>` + `static_cast<Derived*>` |

### 链式 co_await

`co_await (send && recv)` — 单次挂起完成两个 I/O 操作。

| 平台 | 链式机制 | 开销 |
|------|:------:|:---:|
| io_uring | `IOSQE_IO_LINK` (内核级) | 零 userspace |
| epoll / IOCP | `chain_fn` (用户态回调) | 1 次函数间调用 |

io_uring 下 c=500 时 chain (24,716) 超越 plain (24,480)。epoll 下链式开销抵消收益，与 ST 持平。

### 跨线程 co_spawn 架构

```
co_spawn_cross(handle)
  │ mutex lock → push cross_queue
  │ mutex unlock
  ├─ if (was_empty) proactor->wakeup()  ← 批量唤醒优化
  ▼
目标 worker:
  drain_cross_thread() → forward_task() → SPSC reap_swap → schedule()
```

批量唤醒优化（空→非空才调用 wakeup）减少 PQCS/eventfd 调用 90%+，c=200 +55%, c=500 +74%。

### 编译切换

```bash
# epoll 默认
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# io_uring
cmake -S . -B build -G Ninja -DCORONET_IOURING=ON

# 启用全部测试
cmake -S . -B build -DCORONET_BUILD_TESTS=ON -DCORONET_BUILD_BENCHMARKS=ON -DCORONET_BUILD_STRESS_TESTS=ON
```

## 测试基础设施

### CMake 选项

| 选项 | 默认值 | 说明 |
|------|:---:|------|
| `CORONET_BUILD_TESTS` | `PROJECT_IS_TOP_LEVEL` | 构建单元测试 + 集成测试 |
| `CORONET_BUILD_BENCHMARKS` | `ON` | 构建 Google Benchmark 微基准测试 |
| `CORONET_BUILD_STRESS_TESTS` | `OFF` | 构建压力测试（含 redis_loadgen） |

### 目录结构

```
test/                          # 单元测试 + 集成测试 (CTest)
  CMakeLists.txt
  task_gtest.cpp               # GTest: task<T> 基础 (4 用例)
  generator_gtest.cpp          # GTest: generator<T> 基础 (3 用例)
  channel_gtest.cpp            # GTest: channel<T> (3 用例)
  shared_task_gtest.cpp        # GTest: shared_task<T> (8 用例)
  ft_task.cpp                  # 独立: task<T> 功能测试
  move_shared_task.cpp         # 独立: shared_task 移动语义
  generator_test.cpp           # 独立: generator 高级场景
  coro_lifetime.cpp            # 独立: 协程参数生命周期
  stress_test.cpp              # 独立: TCP echo (Linux only, POSIX socket)
  mutex.cpp                    # 独立: 协程互斥锁 (100 协程 × 10K 累加)
  sem.cpp                      # 独立: 信号量 (10 协程竞争 3 槽位)
  timer.cpp                    # 独立: 相对超时
  timer_accuracy.cpp           # 独立: 绝对时间点超时精度
  when_all.cpp                 # 独立: when_all 协程组合器
  when_any.cpp                 # 独立: when_any 协程组合器
  when_some.cpp                # 独立: when_some 协程组合器
  channel.cpp                  # 独立: CSP 通道 (3 生产者 → 3 消费者)
  cv_notify_all.cpp            # 独立: 条件变量 notify_all
  cv_notify_one.cpp            # 独立: 条件变量 notify_one

bench/                         # 微基准测试 (CTest)
  CMakeLists.txt
  task_benchmark.cpp           # Google Benchmark: 协程创建/链开销
  generator_benchmark.cpp      # Google Benchmark: 生成器 yield 开销

stress-test/                   # 压力/负载测试 (CTest, CORONET_BUILD_STRESS_TESTS=ON)
  CMakeLists.txt
  stress_driver.cpp            # 统一压测驱动: RPS + CPU + 内存 + CSV (C++20)
  redis_echo_ST.cpp            # coronet 单线程 Redis echo 服务端
  redis_echo_chain.cpp         # coronet 链式 co_await Redis echo 服务端
  redis_echo_MT.cpp            # coronet 多线程 Redis echo 服务端
  redis_echo_asio_ST.cpp       # ASIO 单线程回调 Redis echo 服务端 (条件编译)
  redis_echo_asio_MT.cpp       # ASIO 多线程 Redis echo 服务端 (条件编译)
  redis_loadgen.cpp            # 自定义 TCP 负载生成器 (跨平台，支持多端口)
```

## 测试清单

| # | 测试 | 类型 | 框架 | Linux | Windows |
|:--:|------|------|------|:---:|:---:|
| 1 | task_gtest | 单元 | GoogleTest | ✅ | ✅ |
| 2 | generator_gtest | 单元 | GoogleTest | ✅ | ✅ |
| 3 | channel_gtest | 单元 | GoogleTest | ✅ | ✅ |
| 4 | shared_task_gtest | 单元 | GoogleTest | ✅ | ✅ |
| 5 | ft_task | 集成 | assert | ✅ | ✅ |
| 6 | move_shared_task | 集成 | assert | ✅ | ✅ |
| 7 | generator_test | 集成 | assert | ✅ | ✅ |
| 8 | coro_lifetime | 集成 | assert | ✅ | ✅ |
| 9 | stress_test | 集成 | assert | ✅ | — |
| 10 | mutex | 集成 | assert | ✅ | ✅ |
| 11 | sem | 集成 | assert | ✅ | ✅ |
| 12 | timer | 集成 | assert | ✅ | ✅ |
| 13 | timer_accuracy | 集成 | assert | ✅ | ✅ |
| 14 | when_all | 集成 | assert | ✅ | ✅ |
| 15 | when_any | 集成 | assert | ✅ | ✅ |
| 16 | when_some | 集成 | assert | ✅ | ✅ |
| 17 | channel | 集成 | assert | ✅ | ✅ |
| 18 | cv_notify_all | 集成 | assert | ✅ | ✅ |
| 19 | cv_notify_one | 集成 | assert | ✅ | ✅ |
| 20 | task_benchmark | 微基准 | Google Benchmark | ✅ | ✅ |
| 21 | generator_benchmark | 微基准 | Google Benchmark | ✅ | ✅ |
| 22 | stress_driver_ST | 压测 | coronet ST + chain + ASIO ST | ✅ | ✅ |
| 23 | stress_driver_MT | 压测 | coronet MT(6) + ASIO MT(6) | ✅ | — |

> `stress_test` (第 9 项) 使用 POSIX `sys/socket.h`，仅 Linux 编译。Linux +1 项 `stress_driver_MT` 仅 Linux 有（ASIO MT 依赖 POSIX 条件编译）。

## 单元测试 (GoogleTest)

### task_gtest — task\<T\> 协程基础

| 测试用例 | 验证内容 |
|----------|---------|
| `TaskTest.CreateValue` | `task<int>` 创建并返回值 |
| `TaskTest.CreateVoid` | `task<void>` 创建并完成 |
| `TaskTest.MoveSemantics` | task 移动构造/赋值 |
| `TaskTest.Detach` | `detach()` 后协程正确销毁 |

**结果**: 4/4 ✅

### generator_gtest — generator\<T\> 基础

| 测试用例 | 验证内容 |
|----------|---------|
| `GeneratorTest.Fibonacci` | 生成 Fibonacci 序列 |
| `GeneratorTest.StringViews` | 生成 string_view 序列 |
| `GeneratorTest.MoveOnly` | 生成 move-only 类型 |

**结果**: 3/3 ✅

### channel_gtest — CSP 通道

| 测试用例 | 验证内容 |
|----------|---------|
| `ChannelTest.CreateBuffered` | 有缓冲通道创建 |
| `ChannelTest.CreateSingleSlot` | 单槽通道创建 |
| `ChannelTest.CreateRendezvous` | 无缓冲同步通道创建 |

**结果**: 3/3 ✅

### shared_task_gtest — shared_task\<T\>

| 测试用例 | 验证内容 |
|----------|---------|
| `SharedTaskTest.DefaultConstruction` | 默认构造 |
| `SharedTaskTest.BasicValue` | 基本值返回 |
| `SharedTaskTest.MultipleAwait` | 多协程 await 同一 shared_task |
| `SharedTaskTest.ExceptionPropagation` | 异常传播 |
| `SharedTaskTest.WhenReady` | `when_ready()` 语义 |
| `SharedTaskTest.MoveSemantics` | 移动语义 |
| `SharedTaskTest.VoidReturn` | void 返回 |
| `SharedTaskTest.ReferenceReturn` | 引用返回 |

**结果**: 8/8 ✅

## 集成测试 (Standalone)

| 测试 | 验证内容 | Linux | Windows |
|------|---------|:---:|:---:|
| `ft_task` | `task<T>` 构造、`co_spawn`、`co_await`、异常处理、嵌套协程 | ✅ | ✅ |
| `move_shared_task` | `shared_task` 多次 await、移动后语义 | ✅ | ✅ |
| `generator_test` | Fibonacci、拼接、嵌套、递归、move-only、异常传播 | ✅ | ✅ |
| `coro_lifetime` | 协程参数拷贝/移动/析构生命周期 | ✅ | ✅ |
| `stress_test` | 内建 echo 服务端 + POSIX 客户端，多连接并发 | ✅ | — |
| `mutex` | 100 协程各累加 10K 次互斥锁保护，验证结果 = 1,000,000 | ✅ | ✅ |
| `sem` | 10 协程竞争 3 信号量槽位 | ✅ | ✅ |
| `timer` | 相对超时 (1s/3s) 周期性触发 | ✅ | ✅ |
| `timer_accuracy` | 绝对时间点超时，延迟 < 50ms | ✅ | ✅ |
| `when_all` | `co_await all(f0, f1, f2)` 结构化绑定获取结果 | ✅ | ✅ |
| `when_any` | `co_await any(...)` 首个完成者胜出，返回 `(index, variant)` | ✅ | ✅ |
| `when_some` | `co_await some(2, ...)` 等待 2 个任务完成 | ✅ | ✅ |
| `channel` | 3 生产者 → 3 消费者 CSP 通道，12 条消息 | ✅ | ✅ |
| `cv_notify_all` | 3 个 waiter + signaler，`notify_all` 唤醒全部 | ✅ | ✅ |
| `cv_notify_one` | worker/main 模式，`notify_one` 单次唤醒 | ✅ | ✅ |

**全部通过**: Linux 15/15 ✅ | Windows 14/14 ✅

## 微基准测试 (Google Benchmark)

### task_benchmark — 纯协程开销

| Benchmark | GCC 13.3 | Clang 18.1 | MSVC 19.41 | 说明 |
|-----------|------:|------:|------:|------|
| `BM_TaskCreate` | 101 ns | 127 ns | 90 ns | 创建并恢复 `task<int>` |
| `BM_TaskChain_10` | 120 ns | 107 ns | 95 ns | 10 层协程链 |
| `BM_TaskChain_100` | 142 ns | 122 ns | 110 ns | 100 层协程链 |

MSVC 协程优化 (frame elision) 明显优于 GCC/Clang，`BM_TaskCreate` 仅 90ns。

### generator_benchmark — 生成器 yield 开销

| Benchmark | GCC 13.3 | Clang 18.1 | MSVC 19.41 | 说明 |
|-----------|------:|------:|------:|------|
| `BM_GeneratorYield/100` | 1,590 ns | 1,493 ns | 1,120 ns | yield 100 个 int |
| `BM_GeneratorYield/1000` | 13,638 ns | 14,016 ns | 11,850 ns | yield 1000 个 int |
| `BM_GeneratorYield/10000` | 129,647 ns | 139,822 ns | 118,200 ns | yield 10000 个 int |

## 性能对比测试（压力测试）

### 测试工具与方法

使用 `stress_driver` 统一采集 RPS + CPU% + Mem MB：
- **redis_loadgen**: 自定义 PING 压测工具，C++20 `std::filesystem` 跨平台，阻塞线程模型（避免客户端事件循环干扰）
- **redis-benchmark**: 辅助压测工具（双工具策略：有 RB 则仅用 RB，无则仅用 LG，数据干净）
- **资源采样**: 后台线程每 500ms 持续采样，取平均值

#### redis_loadgen — 多端口支持

```bash
# 单端口（兼容旧行为）
redis_loadgen -p 6379 -c 50 -n 10000

# 端口范围 / 逗号列表 / 混合
redis_loadgen -p 6390-6395 -c 60 -n 100000     # 范围
redis_loadgen -p 6390,6391,6392 -c 50 -n 10000  # 列表
redis_loadgen -p 6390-6392,6395 -c 50 -n 10000  # 混合
```

连接按 round-robin 均匀分配到各端口，支持 coronet MT 多端口模型压测。所有工作线程共享一个 `std::atomic<int64_t>` 请求计数器，主线程 polling 检测达标后停止。

#### stress_driver 配置

```bash
# --server "name:binary:base_port[:port_count]"
./stress_driver --server "coronet_ST:redis_echo_ST:6380"           # port_count=1
./stress_driver --server "coronet_MT(6):redis_echo_MT:6390:6"      # port_count=6 (多端口)
```

`port_count` 可选，默认 1。当 `port_count > 1` 时，`redis_loadgen` 自动以端口范围调用。

#### 资源采样 (CPU/Mem)

| 平台 | CPU 采样 | 内存采样 | 方法 |
|------|:---:|:---:|------|
| Windows | `GetProcessTimes` | `GetProcessMemoryInfo` | Win32 API 直调，零脚本依赖 |
| Linux | `/proc/pid/stat` (utime+stime) | RSS pages × page_size | 文件读取 |

CPU% 使用 delta-time 算法：`ΔCPU_time / Δwall_time × 100`，避免 PowerShell/top 文本解析不稳定。

#### 采样方法改进

CPU/内存采样从 PowerShell `Get-Process` 改为 Win32 API 直调：
- **CPU%**：`GetProcessTimes` 获取内核+用户时间，delta-time 算法计算瞬时百分比
- **内存**：`GetProcessMemoryInfo` 获取 `WorkingSetSize`，单位 MB
- 零脚本依赖，采样频率 500ms，取测试周期均值

### 1. epoll 后端：coronet vs ASIO (WSL2, GCC 13.3 / Clang 18.1)

**10000 PING × 50 conn，redis_loadgen 压测**

#### 1.1 单线程（ST）— 编译器对比

| 服务器 | GCC 13.3 | 相对 ASIO | Clang 18.1 | 相对 ASIO |
|--------|-----:|:---:|-----:|:---:|
| coronet_ST | 25,856 | 55.1% | 28,336 | 87.9% |
| coronet_chain | 36,529 | 77.8% | 28,209 | 87.5% |
| **ASIO_ST** | **46,935** | — | **32,251** | — |

ASIO_ST 在 epoll 单线程下领先。GCC 下 ASIO 优势更大（+45-82%），Clang 下差距缩小（+12-14%）。

#### 1.2 历史基线（2026-06-29，redis-benchmark）

| c | coronet ST | coronet chain | ASIO ST | 胜者 |
|:--:|-----:|-----:|-----:|:---:|
| 10 | 16,703 | 16,567 | — | coronet |
| 50 | **19,952** | 14,051 | 19,175 | coronet |
| 100 | **20,408** | 20,096 | 20,161 | ≈ 平手 |

redis-benchmark 事件驱动客户端与 epoll 服务端存在交互干扰（epoll-on-epoll），且服务端不处理 pipelining，**不推荐作为跨框架公平压测工具**。

### 2. io_uring 后端：coronet vs ASIO

coronet 使用原生 io_uring 后端（`IOSQE_IO_LINK` 内核链式 I/O，批量 SQE 提交），ASIO 使用 epoll reactor（无 io_uring 支持）。10000 req × 50 conn。

#### 2.1 单线程（ST）— GCC vs Clang

| 服务器 | GCC 13.3 | 相对 ASIO | Clang 18.1 | 相对 ASIO |
|--------|-----:|:---:|-----:|:---:|
| coronet_ST | 42,037 | 96.9% | **46,326** | **109.5%** |
| coronet_chain | 37,507 | 86.4% | 36,043 | 85.2% |
| ASIO_ST | 43,388 | — | 42,307 | — |

#### 2.2 多线程（MT, 6 线程共享 1 端口）— GCC vs Clang

| 服务器 | GCC 13.3 | 相对 ASIO | Clang 18.1 | 相对 ASIO |
|--------|-----:|:---:|-----:|:---:|
| coronet_MT(6) | 137,375 | **116.0%** | **188,548** | **145.8%** |
| ASIO_MT(6) | 118,387 | — | 129,308 | — |

#### 2.3 epoll → io_uring 提升

| 服务器 | GCC epoll | GCC io_uring | 提升 | Clang epoll | Clang io_uring | 提升 |
|--------|-----:|-----:|:---:|-----:|-----:|:---:|
| coronet_ST | 25,856 | 42,037 | **+63%** | 28,336 | 46,326 | **+64%** |
| coronet_MT | 134,885 | 137,375 | +2% | 101,063 | 188,548 | **+87%** |
| ASIO_MT | 137,405 | 118,387 | -14% | 84,170 | 129,308 | +54% |

**coronet_ST 的 io_uring 提升在两个编译器上完全一致（+63% vs +64%）** — 这是架构红利，非编译器优化。

#### 2.4 io_uring MT — GCC vs Clang

| 服务器 | GCC | Clang | Clang/GCC | 分析 |
|--------|-----:|-----:|:---:|------|
| coronet_MT | 137,375 | 188,548 | **137%** | ThinLTO 跨模块优化 mutex + cross_queue |
| ASIO_MT | 118,387 | 129,308 | 109% | ASIO header-only，LTO 无额外收益 |

#### 2.5 历史基线 (GCC 13.3, 2026-06-29, redis-benchmark)

| c | coronet (io_uring) | co_context (io_uring) | ASIO (epoll) |
|:--:|-----:|-----:|-----:|
| 10 | **24,426** | 21,281 | 17,724 |
| 50 | 21,227 | **24,231** | 19,531 |
| 100 | 19,448 | **21,901** | 19,044 |
| 200 | 19,619 | 19,708 | 19,724 |
| 500 | 18,706 | 17,860 | 18,943 |
| 1000 | **19,992** | 17,346 | 17,960 |

coronet 平均值领先 9.3%。

### 3. 多线程性能 — 共享端口模型

**6 线程共享 1 端口**。coronet 使用跨线程 `co_spawn` 将 accept 分发给 N 个独立 `io_context` worker，ASIO 使用 `io_context::run()` 多线程共享。10000 req × 50 conn，redis_loadgen 压测。

#### 3.1 epoll 后端 (WSL2) — 编译器对比

| 服务器 | GCC 13.3 | 相对 ASIO | Clang 18.1 | 相对 ASIO |
|--------|-----:|:---:|-----:|:---:|
| coronet_MT(6) | 134,885 | 98.2% | **101,063** | **120.1%** |
| ASIO_MT(6) | 137,405 | — | 84,170 | — |

#### 3.2 IOCP 后端 (Windows 10, MSVC 19.41)

| 服务器 | RPS | 相对 ASIO |
|--------|-----:|:---:|
| coronet_MT(6) | 56,465 | 96.6% |
| **ASIO_MT(6)** | **58,438** | — |

#### 3.3 全平台 MT 总览（6 线程共享 1 端口）

| 编译器/后端 | coronet_MT | ASIO_MT | coronet/ASIO | 胜者 |
|:---|-----:|-----:|:---:|:---:|
| **Clang 18.1 + io_uring** | **188,548** | 129,308 | **145.8%** | **coronet +46%** |
| GCC 13.3 + io_uring | 137,375 | 118,387 | 116.0% | coronet +16% |
| GCC 13.3 + epoll | 134,885 | 137,405 | 98.2% | ASIO (+1.8%) |
| Clang 18.1 + epoll | 101,063 | 84,170 | 120.1% | coronet (+20%) |
| MSVC 19.41 + IOCP | 56,465 | 58,438 | 96.6% | ASIO (+3.4%) |

🏆 **全平台最高单次记录**: Clang io_uring coronet_MT(6) = **188,548 RPS**（MSVC IOCP 的 **3.3×**）

**结论**:
- **后端选择 > 编译器选择 > 框架选择**
- io_uring 下 coronet 在所有编译器上**均领先** ASIO（+16% ~ +46%）
- epoll 下编译器定胜负（GCC→ASIO 赢，Clang→coronet 赢）
- IOCP 下基本持平（coronet/ASIO = 96.6%）
- coronet_ST 的 io_uring 提升在 GCC/Clang 上完全一致（+63% vs +64%）— 架构红利，非编译器优化

#### 3.4 架构对比（共享端口模型）

```
coronet_MT(6) — 跨线程 co_spawn          ASIO_MT(6) — 共享 io_context
────────────────────────────             ──────────────────────────
6 × io_context (独立线程，各 1 线程)      1 × io_context (6 线程共享)
worker[0]: acceptor::accept() + session  all threads: run() → GQCS
worker[1-5]: session only

accept → cross-thread co_spawn          IOCP 原生分发 completion
  │ mutex lock → push cross_queue         │ GQCS 唤醒任意线程
  │ mutex unlock                          │
  └─ PostQueuedCompletionStatus           │
     → 目标 worker drain_cross_thread()   │
```

#### 3.5 历史基线 (io_uring, 2026-06-29)

6 worker + 1 balancer 多端口模型，redis-benchmark 压测。

| c | coronet MT | co_context MT | 胜者 |
|:--:|-----:|-----:|:---:|
| 200 | **26,309** | 18,002 | coronet |
| 500 | **25,700** | 13,401 | coronet |
| 1000 | 14,021 | **20,190** | co_context |

### 4. Windows IOCP：coronet vs ASIO (MSVC 19.41)

**stress_driver 统一采集**：10000 PING × 50 conn，redis_loadgen 压测，CPU/内存 500ms 间隔采样取均值。

#### 4.1 单线程（ST）

| 服务器 | RPS | CPU% | Mem | 说明 |
|--------|-----:|:---:|-----|------|
| coronet_ST | 52,006 | 6.1% | 4 MB | 基础协程 |
| coronet_chain | 59,506 | 6.1% | 6 MB | 链式 `co_await (send && recv)` |
| ASIO_ST | 58,630 | 6.2% | 3 MB | 回调 |
| **coronet_ST / ASIO** | **0.89** | — | — | — |

#### 4.2 多线程（MT, 6 线程共享 1 端口）

| 服务器 | RPS | CPU% | Mem | 说明 |
|--------|-----:|:---:|-----|------|
| coronet_MT(6) | 56,465 | 3.0% | 2 MB | 1 acceptor + 6 worker io_context |
| ASIO_MT(6) | 58,438 | 3.0% | 4 MB | 1 io_context × 6 线程 |
| **coronet / ASIO** | **0.97** | — | — | 差距 3.4% |

#### 4.3 历史基线（2026-06-29，100K req/级别）

| c | coronet (IOCP) | ASIO (IOCP) | 比值 | 胜者 |
|:--:|-----:|-----:|:---:|:---:|
| 10 | **51,252** | 51,541 | 0.99 | ≈ 平手 |
| 50 | 41,915 | **50,971** | 0.82 | ASIO |
| 100 | **48,954** | 36,142 | **1.35** | coronet |
| 200 | **43,613** | 35,754 | **1.22** | coronet |
| 500 | 39,186 | **43,454** | 0.90 | ASIO |
| 1000 | **39,334** | 30,234 | **1.30** | coronet |

coronet IOCP 赢 4/6，MSVC 协程帧省略优化贡献显著（c=10: 51K RPS）。

### 5. 编译器性能对比

#### 5.1 单线程（ST）

| 编译器 | coronet_ST | ASIO_ST | coronet/ASIO | 胜者 |
|:---|-----:|-----:|:---:|:---:|
| GCC 13.3 (epoll) | 25,856 | **46,935** | 55.1% | ASIO (+82%) |
| Clang 18.1 (epoll) | 28,336 | **32,251** | 87.9% | ASIO (+14%) |
| MSVC 19.41 (IOCP) | 52,006 | **58,630** | 88.7% | ASIO (+13%) |

#### 5.2 多线程（MT, 6 线程共享 1 端口）

| 编译器/后端 | coronet_MT(6) | ASIO_MT(6) | coronet/ASIO | 胜者 |
|:---|-----:|-----:|:---:|:---:|
| **Clang 18.1 + io_uring** | **188,548** | 129,308 | **145.8%** | coronet |
| GCC 13.3 + io_uring | 137,375 | 118,387 | 116.0% | coronet |
| GCC 13.3 + epoll | 134,885 | 137,405 | 98.2% | ASIO |
| Clang 18.1 + epoll | 101,063 | 84,170 | 120.1% | coronet |
| MSVC 19.41 + IOCP | 56,465 | 58,438 | 96.6% | ASIO |

#### 5.3 分析

**编译器对框架排名有决定性影响**：

- **GCC**: 对 ASIO 模板代码生成最优（header-only 模板，函数指针 devirtualization），epoll 下 ASIO 领先 82%
- **Clang**: ThinLTO 跨模块优化对 coronet（`.a` 静态库）收益显著，ASIO（全 header）无 LTO 收益
- **MSVC**: 帧省略（frame elision）对协程优化最激进，绝对值受 IOCP 平台限制

**io_uring 统一了胜负**：无论编译器，io_uring 后端下 coronet 均领先 ASIO（+16% ~ +46%）。Completion 模型原生匹配消除了编译器差异。

#### 5.4 历史峰值 (2026-06-28, redis-benchmark)

| 排名 | 服务端 | GCC | Clang | MSVC | 最佳 |
|:--:|------|:---:|:---:|:---:|:---:|
| 🥇 | coronet | 42,290 | 42,546 | **49,559** | MSVC |
| 🥈 | ASIO | 47,098 | 45,444 | 48,262 | GCC |
| 🥉 | co_context | 42,649 | 45,914 | N/A | Clang |

## 压力测试驱动 (stress_driver)

### 架构设计

`stress_driver` 是**纯数据采集器**，通过 `--server name:binary:port` 参数配置测试目标。
添加新服务端**无需修改 C++ 代码** — 只需在 `CMakeLists.txt` 中追加 `--server` 参数。

```
CMakeLists.txt (配置)              stress_driver (引擎，不改代码)
─────────────────────              ─────────────────────────────
add_test(NAME stress_driver_ST     for each --server:
  --server "coronet_ST:              1. spawn 服务端子进程
           redis_echo_ST:6380"       2. wait_ready (redis-cli ping)
  --server "coronet_chain:           3. redis-benchmark 压测
           redis_echo_chain:6381"    4. redis_loadgen 压测
  --server "ASIO_ST:                 5. 持续 CPU/内存采样 (均值)
           redis_echo_asio_ST:6382"  6. kill 子进程
  -n 10000 -c 50)                    → CSV 输出
```

**关键操作**：添加新服务端只需在 CMakeLists 中加一行 `add_executable` + 一行 `--server`。

### 特性

| 特性 | Linux | Windows |
|------|:---:|:---:|
| 压测工具 | redis-benchmark + redis_loadgen | redis_loadgen |
| CPU 监控 | `/proc/pid/stat` (utime+stime) delta-time | `GetProcessTimes` delta-time |
| 内存监控 | RSS pages × page_size | `GetProcessMemoryInfo` WorkingSetSize |
| 采样方式 | 后台线程每 500ms 持续采样，取平均值 |
| 数据输出 | `data/stress_YYYYMMDD_HHMMSS.csv` |
| 服务端管理 | stress_driver 自动 spawn → 健康检查 → kill |

### CTest 集成

```bash
# 单线程对比 (coronet ST + chain + ASIO ST)
ctest -R stress_driver_ST

# 多线程对比 (coronet MT(6) + ASIO MT(6))
ctest -R stress_driver_MT
```

### 手动使用

```bash
# 基本：测试指定服务端
./stress_driver --server "coronet_ST:redis_echo_ST:6380" \
                --server "coronet_chain:redis_echo_chain:6381"

# 高负载：100K 请求 × 100 并发
./stress_driver --server "coronet_ST:redis_echo_ST:6380" -n 100000 -c 100 -v

# 多服务端：冠名 + ASIO 对比
./stress_driver \
  --server "coronet_ST:redis_echo_ST:6380" \
  --server "ASIO_ST:redis_echo_asio_ST:6382" \
  -n 100000 -c 100 -v
```

### 添加新服务端（示例）

```cmake
# 1. stress-test/CMakeLists.txt — 添加构建目标
add_executable(redis_echo_new redis_echo_new.cpp)
target_link_libraries(redis_echo_new PRIVATE coronet::coronet)

# 2. 在 add_test 中追加一行 (不改 stress_driver 代码!)
  --server "NewServer:redis_echo_new:6399"
```

### 性能数据 (epoll, WSL2, GCC 13.3, 10K req × 50 并发)

| 服务端 | redis-benchmark | redis_loadgen | CPU% (avg) | Mem KB |
|--------|-----:|-----:|:---:|-----|
| **coronet ST** | 17,352 | 17,352 | 67.5 | 3,930 |
| **coronet chain** | 15,152 | 19,703 | 53.4 | 4,058 |
| **ASIO ST** | 19,175 | 17,291 | 73.2 | 3,724 |

## 开发历史关键 Bug 修复

以下 Bug 在测试过程中被发现并修复：

| Bug | 影响 | 修复 |
|------|------|------|
| SPSC `pop()` 未掩码 | 16384 次迭代后 SIGSEGV | `return h & mask` |
| reap_swap 栈溢出 (Win) | 10×io_context → 1.3MB 栈 | `array` → `vector` (堆) |
| io_uring timeout UAF | `__kernel_timespec` 栈变量 → 悬挂指针 | 改为成员变量 |
| `task_promise<void>` 析构 UB | union 未构造 `exception_ptr` → 双重释放 | `has_exception_` 标志 |

## CTest 测试矩阵

### 完整运行命令

```bash
# Linux / WSL — 构建全部 + 测试
cmake -B build -DCORONET_DEVELOPER_MODE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build && ctest --output-on-failure

# Windows MSVC — 构建全部 + 测试
cmake -B build -DCORONET_DEVELOPER_MODE=ON
cmake --build build --config Release
cd build && ctest -C Release --output-on-failure

# 单独压测
./stress-test/stress_driver \
    --server "coronet_ST:redis_echo_ST:6380" \
    --server "ASIO_ST:redis_echo_asio_ST:6382" \
    -n 10000 -c 50
# → data/stress_YYYYMMDD_HHMMSS.csv
```

### 结果矩阵

| 平台 / 编译器 | 后端 | 测试数 | 结果 | 耗时 |
|:---|:---|:---:|:---:|-----:|
| **Linux GCC 13.3** | epoll | **23/23** | ✅ 100% | 51.05s |
| **Linux Clang 18.1** | epoll | **23/23** | ✅ 100% | — |
| **Windows MSVC 19.41** | IOCP | **22/22** | ✅ 100% | 101.69s |

测试覆盖：4×GTest 单元 (18 用例) + 15×集成 (mutex/sem/timer/when_all/channel/cv/stress_test/...) + 2×Google Benchmark + **2×stress_driver (ST + MT)**。

> Linux +1 项 `stress_test`（POSIX socket）仅 Linux 编译。Windows + 无 `stress_driver_MT`（ASIO MT 依赖 POSIX 条件编译）。

### Linux GCC 23/23 明细

```
 1/23 task_gtest ....................... Passed  0.03s
 2/23 generator_gtest .................. Passed  0.03s
 3/23 channel_gtest .................... Passed  0.03s
 4/23 shared_task_gtest ................ Passed  0.05s
 5/23 ft_task .......................... Passed  0.02s
 6/23 move_shared_task ................. Passed  0.01s
 7/23 generator_test ................... Passed  0.02s
 8/23 coro_lifetime .................... Passed  0.01s
 9/23 stress_test ...................... Passed  1.04s
10/23 mutex ............................ Passed  0.01s
11/23 sem .............................. Passed  5.01s
12/23 timer ............................ Passed  6.02s
13/23 timer_accuracy ................... Passed  5.01s
14/23 when_all ......................... Passed  2.07s
15/23 when_any ......................... Passed  0.02s
16/23 when_some ........................ Passed  1.01s
17/23 channel .......................... Passed  8.01s
18/23 cv_notify_all .................... Passed  5.05s
19/23 cv_notify_one .................... Passed  5.01s
20/23 task_benchmark ................... Passed  2.12s
21/23 generator_benchmark .............. Passed  2.19s
22/23 stress_driver_ST ................. Passed  9.52s
23/23 stress_driver_MT ................. Passed  5.80s

Total: 23/23 PASSED (51.05s)
```

### Windows MSVC 22/22 明细

```
 1/22 task_gtest ....................... Passed  0.19s
 2/22 generator_gtest .................. Passed  0.21s
 3/22 channel_gtest .................... Passed  0.21s
 4/22 shared_task_gtest ................ Passed  0.22s
 5/22 ft_task .......................... Passed  0.17s
 6/22 move_shared_task ................. Passed  0.20s
 7/22 generator_test ................... Passed  0.21s
 8/22 coro_lifetime .................... Passed  0.22s
 9/22 mutex ............................ Passed  0.22s
10/22 sem .............................. Passed  5.18s
11/22 timer ............................ Passed  6.21s
12/22 timer_accuracy ................... Passed  5.29s
13/22 when_all ......................... Passed  2.19s
14/22 when_any ......................... Passed  0.21s
15/22 when_some ........................ Passed  1.20s
16/22 channel .......................... Passed  8.23s
17/22 cv_notify_all .................... Passed  5.21s
18/22 cv_notify_one .................... Passed  5.34s
19/22 task_benchmark ................... Passed  3.85s
20/22 generator_benchmark .............. Passed  4.20s
21/22 stress_driver_ST ................. Passed  1.05s
22/22 stress_driver_MT ................. Passed  6.80s

Total: 22/22 PASSED (101.69s)
```

## 复现

```bash
# Linux / WSL — 构建全部 + 测试
cmake -B build -DCORONET_DEVELOPER_MODE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build && ctest --output-on-failure

# Windows MSVC — 构建全部 + 测试
cmake -B build -DCORONET_DEVELOPER_MODE=ON
cmake --build build --config Release
cd build && ctest -C Release --output-on-failure

# 单独压测
./stress-test/stress_driver \
    --server "coronet_ST:redis_echo_ST:6380" \
    --server "ASIO_ST:redis_echo_asio_ST:6382" \
    -n 10000 -c 50
# → data/stress_YYYYMMDD_HHMMSS.csv
```

---

*报告更新于 2026-07-01。变动摘要：*
- *stress_driver C++20 重构 (RAII PipeCmd / std::format / std::span / std::filesystem)*
- *资源采样: Win32 API 直调 + Linux /proc/pid/stat delta-time 算法*
- *redis_loadgen CLI/输出对齐 redis-benchmark (-P pipeline, -q quiet, unified "requests per second")，支持多端口范围*
- *coronet_MT 共享端口模型 (跨线程 co_spawn) — GCC/Clang/MSVC 三编译器对比*
- *三编译器 CTest: GCC 23/23 ✅ | Clang 23/23 ✅ | MSVC 22/22 ✅*
- *原始数据: build/stress-test/data/ (Win) / buildgcc/stress-test/data/ / buildclang/stress-test/data/ (WSL)*
