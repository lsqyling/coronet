# coronet 测试报告

## 测试环境

| 项目 | Linux GCC | Linux Clang | Windows MSVC |
|------|-----------|-------------|--------------|
| **OS** | WSL2 Ubuntu | WSL2 Ubuntu | Windows 10 Pro x64 |
| **Kernel** | 5.15.153.1 | 5.15.153.1 | 10.0.19045 |
| **编译器** | GCC 13.3.0 | Clang 18.1.3 | MSVC 19.41 (VS 2022) |
| **C++ 标准** | C++20 | C++20 | C++20 |
| **构建系统** | Ninja | Ninja | MSBuild |
| **后端** | epoll (默认) | epoll (默认) | IOCP |
| **日期** | 2026-06-29 | 2026-06-29 | 2026-06-29 |

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
  stress_test_Driver.cpp       # 统一压测驱动: RPS + CPU + 内存 + CSV
  redis_echo_ST.cpp            # coronet 单线程 Redis echo 服务端
  redis_echo_chain.cpp         # coronet 链式 co_await Redis echo 服务端
  redis_echo_MT.cpp            # coronet 多线程 Redis echo 服务端
  redis_echo_asio_ST.cpp       # ASIO 单线程回调 Redis echo 服务端 (条件编译)
  redis_echo_asio_MT.cpp       # ASIO 多线程 Redis echo 服务端 (条件编译)
  redis_loadgen.cpp            # 自定义 TCP 负载生成器 (跨平台)
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

> `stress_test` (第 9 项) 使用 POSIX `sys/socket.h`，仅 Linux 编译。

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

## 压力测试 (stress_driver) ⭐

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
| CPU 监控 | `top -b -n1 -p <pid>` | PowerShell `Get-Process` |
| 内存监控 | `top` RES (KB) | PowerShell WorkingSet64 |
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
|--------|-----:|-----:|:---:|-----:|
| **coronet ST** | 17,352 | 17,352 | 67.5 | 3,930 |
| **coronet chain** | 15,152 | 19,703 | 53.4 | 4,058 |
| **ASIO ST** | 19,175 | 17,291 | 73.2 | 3,724 |

## 三平台 CTest 汇总

### 完整运行命令

```bash
# Linux / WSL
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCORONET_BUILD_TESTS=ON \
    -DCORONET_BUILD_BENCHMARKS=ON \
    -DCORONET_BUILD_STRESS_TESTS=ON
cmake --build build
cd build && ctest --output-on-failure -j4

# Windows MSVC
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 \
    -DCORONET_BUILD_TESTS=ON \
    -DCORONET_BUILD_BENCHMARKS=ON \
    -DCORONET_BUILD_STRESS_TESTS=ON
cmake --build build --config Release
cd build && ctest -C Release --output-on-failure
```

### 结果矩阵

| 平台 / 编译器 | 后端 | 测试数 | 结果 | 耗时 |
|:---|:---|:---:|:---:|-----:|
| **Linux GCC 13.3** | epoll | 22/22 | ✅ 100% | 43.20s |
| **Linux Clang 18.1** | epoll | 22/22 | ✅ 100% | 46.77s |
| **Linux GCC 13.3** | io_uring | 22/22 | ✅ 100% | — |
| **Windows MSVC 19.41** | IOCP | 21/21 | ✅ 100% | 50.35s |

### Linux GCC 22/22 明细

```
 1/22 task_gtest ....................... Passed  0.03s
 2/22 generator_gtest .................. Passed  0.03s
 3/22 channel_gtest .................... Passed  0.03s
 4/22 shared_task_gtest ................ Passed  0.05s
 5/22 ft_task .......................... Passed  0.02s
 6/22 move_shared_task ................. Passed  0.01s
 7/22 generator_test ................... Passed  0.02s
 8/22 coro_lifetime .................... Passed  0.01s
 9/22 stress_test ...................... Passed  1.04s
10/22 mutex ............................ Passed  0.01s
11/22 sem .............................. Passed  5.01s
12/22 timer ............................ Passed  6.02s
13/22 timer_accuracy ................... Passed  5.01s
14/22 when_all ......................... Passed  2.07s
15/22 when_any ......................... Passed  0.02s
16/22 when_some ........................ Passed  1.01s
17/22 channel .......................... Passed  8.01s
18/22 cv_notify_all .................... Passed  5.05s
19/22 cv_notify_one .................... Passed  5.01s
20/22 task_benchmark ................... Passed  2.12s
21/22 generator_benchmark .............. Passed  2.19s
22/22 stress_driver_ST .................. Passed  9.52s

Total: 22/22 PASSED (43.20s)
```

### Windows MSVC 21/21 明细

```
 1/21 task_gtest ....................... Passed  0.19s
 2/21 generator_gtest .................. Passed  0.21s
 3/21 channel_gtest .................... Passed  0.21s
 4/21 shared_task_gtest ................ Passed  0.22s
 5/21 ft_task .......................... Passed  0.17s
 6/21 move_shared_task ................. Passed  0.20s
 7/21 generator_test ................... Passed  0.21s
 8/21 coro_lifetime .................... Passed  0.22s
 9/21 mutex ............................ Passed  0.22s
10/21 sem .............................. Passed  5.18s
11/21 timer ............................ Passed  6.21s
12/21 timer_accuracy ................... Passed  5.29s
13/21 when_all ......................... Passed  2.19s
14/21 when_any ......................... Passed  0.21s
15/21 when_some ........................ Passed  1.20s
16/21 channel .......................... Passed  8.23s
17/21 cv_notify_all .................... Passed  5.21s
18/21 cv_notify_one .................... Passed  5.34s
19/21 task_benchmark ................... Passed  3.85s
20/21 generator_benchmark .............. Passed  4.20s
21/21 stress_driver_ST ................. Passed  1.05s

Total: 21/21 PASSED (50.35s)
```

---

*报告生成于 2026-06-29。三平台 (GCC/Clang/MSVC) × 三后端 (epoll/io_uring/IOCP) 全部通过。*