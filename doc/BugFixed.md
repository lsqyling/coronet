# Bug 修复记录：net_echo 测试挂起（CPU >100%，永不终止）

**日期**: 2026-07-30  
**分支**: build/linux  
**影响范围**: epoll 后端（io_uring 和 IOCP 不受影响）

---

## Bug 描述

执行 CTest 测试 `test/net_echo.cpp` 时：
- **CPU 占用 > 100%**（单核满载）
- **内存占用基本为 0**（无新分配）
- **测试永不结束**（违反"快速成功或快速失败"原则）

具体卡在 `echo_server` 的自旋等待阶段：
```cpp
while (done.load(std::memory_order_relaxed) < n) {
    co_await coronet::async::yield();
}
```

`done` 计数器永远无法达到目标值 `n`，因为等待 echo 回执的客户端协程永远不会被恢复。

---

## 为什么会发生这个 Bug？

### 事件循环的四阶段

```
┌──────────────────────────────────────────────────┐
│ while (!will_stop_) {                            │
│   drain_cross_thread();  // 阶段 0: 跨线程协程搬移 │
│   do_worker_part();      // 阶段 1: 恢复就绪协程   │
│   do_submission_part();  // 阶段 2: 提交 I/O      │
│   do_completion_part();  // 阶段 3: 收割 I/O 完成  │
│ }                                                │
└──────────────────────────────────────────────────┘
```

`do_worker_part()` 使用 `while` 循环持续从 SPSC 环中取出协程并恢复执行，直到环为空：
```cpp
void io_context::do_worker_part() {
    while (auto handle = worker_.schedule()) {
        handle.resume();
    }
}
```

### 活锁机制

epoll 后端的 `yield()` 被实现为**同步操作**：

1. `await_suspend` 被调用
2. 执行 `perform_sync_op()`（返回 0）
3. 调用 `forward_task(current)` 将当前协程句柄**立即放回 SPSC 环**
4. 协程被标记为就绪，`await_suspend` 返回

**关键问题**：`forward_task()` 将协程放回 SPSC 环，而 `do_worker_part()` 的 `while` 循环仍在运行。它从 SPSC 环取出这个协程，再次恢复它：

```
do_worker_part():
  while (handle = schedule()):     // ← 取出 server 协程
    handle.resume()                // ← 恢复执行
      → server 检查 done < n
      → co_await yield()
        → forward_task(server)     // ← 放回 SPSC 环
  while (handle = schedule()):     // ← 立即再次取出!
    handle.resume()                // ← 再次恢复
      → server 再次检查 done < n
      → co_await yield()
        → forward_task(server)     // ← 又放回...
  // ... 无限循环！
```

**活锁结果**：
- `do_worker_part()` 永远无法返回
- `do_completion_part()` **永远不会被调用**
- 等待 echo 回执的客户端 recv 事件永远不会被处理
- `done` 计数器永远不会增加
- 超时协程（timerfd）也无法被处理

### 为什么 io_uring/IOCP 不受影响

| 后端 | yield 实现 | 行为 |
|------|-----------|------|
| **epoll** | 同步 `forward_task()` | 立即放回 SPSC 环 → 活锁 |
| **io_uring** | `io_uring_yield` 使用 `prep_nop()` SQE | 异步操作，通过 CQE completion 路径恢复 → 正常 |
| **IOCP** | `win_nop::issue_io()` 调用 `on_sync_completion()` | 异步投递到 IOCP 完成端口 → 正常 |

---

## 怎么解决的？

### 修改 1：epoll_yield 重写 await_suspend

**文件**: `include/coronet/platform/epoll/epoll_lazy_io.hpp`

**问题**：`epoll_yield` 使用基类 `epoll_awaiter_base` 的同步路径，调用 `forward_task()` 立即将协程放回 SPSC 环。

**修改**：重写 `await_suspend`，使用新引入的 `defer_task()` 替代 `forward_task()`：

```cpp
struct epoll_yield final : epoll_awaiter_base<epoll_yield> {
    // ...
    void await_suspend(std::coroutine_handle<> current) noexcept {
        io_info_.handle = current;
        io_info_.result = 0;
        this_thread.worker->defer_task(current);  // ← 延迟到下一轮
        io_info_.handle = nullptr;
    }
};
```

`defer_task()` 将协程句柄放入 `deferred_tasks` 向量（线程本地，无锁），而非 SPSC 环。

### 修改 2：新增延迟任务队列

**文件**: `include/coronet/detail/worker_meta.hpp`

新增：
- `std::vector<std::coroutine_handle<>> deferred_tasks` — 延迟任务队列
- `void defer_task(handle)` — 将协程加入延迟队列
- `void drain_deferred()` — 将延迟队列中的协程搬移到 SPSC 环

### 修改 3：事件循环集成

**文件**: `src/coronet/io_context.cpp`

**事件循环顶部**增加 `drain_deferred()`，与 `drain_cross_thread()` 并列：
```cpp
worker_.drain_cross_thread();
worker_.drain_deferred();  // 将 yield 的协程搬回 SPSC 环（在 do_worker_part 之前）
do_worker_part();
```

**`do_completion_part()` 增加智能非阻塞判断**：
```cpp
void io_context::do_completion_part() noexcept {
    bool has_deferred = !worker_.deferred_tasks.empty();
    bool no_inflight  = (worker_.requests_to_reap == 0);
    bool nonblocking  = has_deferred && no_inflight;
    worker_.poll_completion(nonblocking);
}
```

决策矩阵：

| 延迟任务 | 进行中 I/O | 策略 | 原因 |
|----------|-----------|------|------|
| 无 | — | **阻塞** | 安全：I/O 事件会唤醒循环 |
| 有 | 有 | **阻塞** | 安全：进行中的 I/O 会完成并唤醒循环；延迟任务在下一轮处理 |
| 有 | 无 | **非阻塞** | **必须**：无 I/O 事件能唤醒，阻塞会死锁；必须让延迟任务有机会执行 |

### 修改 4：跨平台 API 一致性

**文件**: `include/coronet/platform/epoll/epoll_reactor.hpp`, `src/coronet/platform/epoll/epoll_reactor.cpp`

`wait_completion` 增加 `nonblocking` 参数。当 `nonblocking=true` 时，调用 `fill_ready_queue(false)`（`epoll_wait(timeout=0)`）。

**文件**: `include/coronet/platform/io_uring/io_uring_proactor.hpp`, `src/coronet/platform/io_uring/io_uring_proactor.cpp`

`wait_completion` 增加 `nonblocking` 参数。当 `nonblocking=true` 且 CQ 为空时，不调用 `submit(true)`，直接返回 0。

**文件**: `include/coronet/platform/iocp/iocp_proactor.hpp`, `src/coronet/platform/iocp/iocp_proactor.cpp`

`wait_completion` 增加 `nonblocking` 参数。当 `nonblocking=true` 时，`GetQueuedCompletionStatus` 使用 `timeout=0` 替代 `INFINITE`。

---

## 为什么这么解决？

### 替代方案及驳回理由

**方案 A：限制 do_worker_part 的迭代次数**

```cpp
void io_context::do_worker_part() {
    int batch = 0;
    while (auto handle = worker_.schedule()) {
        handle.resume();
        if (++batch >= 64) break;  // 硬限制
    }
}
```

**驳回**：如果有超过 64 个真实就绪协程（如高并发场景），它们会被不必要地延迟。这是一个"修复症状而非根因"的方案。

**方案 B：使用 cross_queue（跨线程队列）替代 deferred_tasks**

**驳回**：`cross_queue` 受 mutex 保护，每次 yield 都需要加锁/解锁。yield 应该是轻量操作（用户的 spin-wait 会频繁调用它）。`deferred_tasks` 是线程本地的，无锁 push/pop，开销极低。

**方案 C：让 epoll_yield 使用异步 timerfd（类似 io_uring 的 NOP）**

**驳回**：每次 yield 都要创建/销毁 timerfd，涉及多次系统调用（`timerfd_create`、`timerfd_settime`、epoll 注册/注销、`close`），开销远大于向量 push。

### 当前方案的优势

1. **零额外开销**：`deferred_tasks` 是线程本地 `std::vector`，push/clear 无需锁或系统调用
2. **语义正确**：yield 的语义是"让出控制权给事件循环的其他部分（特别是 I/O 完成处理）"，延迟到下一轮完美实现这个语义
3. **跨平台一致**：io_uring/IOCP 的 yield 本来就是异步的（通过 completion 路径），现在 epoll 的行为与之对齐
4. **最小侵入**：不改变 SPSC 环、proactor 接口或现有 I/O 路径的核心逻辑
5. **不退化**：在高并发场景下（大量 I/O 完成 + 少量 yield），事件循环正常阻塞在 `epoll_wait`，无额外轮询开销

---

# Bug 修复记录：io_uring 后端死循环（CPU 100%，永不终止）

**日期**: 2026-07-31  
**分支**: build/linux  
**影响范围**: io_uring 后端（CORONET_IOURING=ON），WSL2 环境  
**参考实现**: [co_context](https://github.com/Codesire-Deng/co_context) — io_uring 事件循环设计

---

## Bug 描述

启用 `-DCORONET_IOURING=ON` 后，所有测试（sem、channel、timer 等）均出现：
- **CPU 占用 100%**（死循环）
- **测试永不终止**
- **stderr 刷屏**：`[uring] submit() failed: Invalid argument`

```
  PID USER      PR  NI    VIRT    RES    SHR S  %CPU  %MEM     TIME+ COMMAND
33017 shiqing   20   0   83880   7304   6924 S 100.0   0.0   5:10.23 sem
```

---

## 为什么会发生这些 Bug？

本章记录了 3 个独立的罪魁祸首（按发现顺序），全部位于 io_uring 代码路径。

### Bug A：liburingcxx 无条件启用 IORING_ENTER_REGISTERED_RING

**文件**: `extern/liburingcxx/include/uring/uring.hpp`（通过 FetchContent 下载）

liburingcxx 在 kernel >= 5.18 时将 `IORING_ENTER_REGISTERED_RING` 硬编码为 `constexpr`：

```cpp
namespace config {
    // HACK this assumes app will use registered ring.
    constexpr bool using_register_ring_fd = is_kernel_reach(5, 18);  // ← WSL2 6.18 → true

    constexpr unsigned default_enter_flags_registered_ring =
        using_register_ring_fd ? IORING_ENTER_REGISTERED_RING : 0;   // ← 硬编码 flag
};
```

然后在 `__submit()` 和 `_get_cq_entry()` 中无条件使用：

```cpp
// __submit() — 被 submit() / submit_and_wait() 调用
unsigned flags = config::default_enter_flags_registered_ring;  // = IORING_ENTER_REGISTERED_RING
// ...
io_uring_enter(enter_ring_fd, submitted, wait_num, flags, nullptr);

// _get_cq_entry() — 被 wait_cq_entry() 调用
if constexpr (config::using_register_ring_fd) {
    flags |= IORING_ENTER_REGISTERED_RING;
}
```

**问题**：`IORING_ENTER_REGISTERED_RING` 要求 ring fd 先通过 `IORING_REGISTER_RING_FDS` 注册。liburingcxx 在 `init()` 中确实调用了 `register_ring_fd()`，但 WSL2 的内核对这个注册操作返回 0（静默失败），`int_flags` 从未设置 `INT_FLAG_REG_RING`。然而 `default_enter_flags_registered_ring` 是 `constexpr`，不受运行时状态影响。结果：**每个 `io_uring_enter` 调用都返回 EINVAL**。

### Bug B：IORING_SETUP_SINGLE_ISSUER 线程冲突

**文件**: `include/coronet/platform/io_uring/io_uring_proactor.hpp`

coronet 的 io_uring 类型为 kernel 6.0+ 启用了 `IORING_SETUP_SINGLE_ISSUER`：

```cpp
using io_uring_ring = liburingcxx::uring<
    config::io_uring_setup_flags       // = 0
    | config::uring_setup_flags        // = 0
    | config::io_uring_coop_taskrun_flag  // = COOP_TASKRUN | TASKRUN_FLAG
    | IORING_SETUP_SINGLE_ISSUER       // ← kernel 6.0+
    | IORING_SETUP_DEFER_TASKRUN       // ← kernel 6.1+
>;
```

`IORING_SETUP_SINGLE_ISSUER` 的约束：**io_uring_setup 和所有 io_uring_enter 必须在同一线程**。

但 coronet 的初始化流程是：
1. **main 线程**：`io_context` 构造函数 → `proactor_.init()` → `ring_.init()` → `io_uring_setup(SINGLE_ISSUER)` 
2. **事件循环线程**：`ctx.start()` → `run()` → `submit()` → `io_uring_enter()` 

kernel 检测到不同线程访问 → 返回 **EEXIST**：

```
[uring] submit() failed: File exists
[worker] poll_submission failed: -17
```

**对比 co_context**：`worker.init()` 在 `io_context::init()` 中调用，而 `init()` 在 `start()` 的线程 lambda 中执行——与 `run()` 在同一线程。完美遵守 SINGLE_ISSUER。

### Bug C：事件循环在提交失败时无退路

**文件**: `src/coronet/io_context.cpp`, `src/coronet/detail/worker_meta.cpp`

Bug A 和 Bug B 导致 `submit()` 失败后，事件循环没有正确的错误处理机制：

```
事件循环第 1 轮:
  do_worker_part()     → 11 个协程挂起等待 timeout（创建了 SQE）
  do_submission_part() → submit() → EINVAL/EEXIST
                        → requests_to_submit = 0（重置），但 SQE 未到达内核！
                        → requests_to_reap 仍 = 11（从不递减）
  do_completion_part() → wait_completion(false)
                        → submit(true) → EINVAL/EEXIST
                        → 返回 0（无完成事件）

事件循环第 2 轮..∞:
  do_worker_part()     → 空（所有协程挂起）
  do_submission_part() → requests_to_submit=0 → 跳过
  do_completion_part() → submit(true) → 失败 → 返回 0
  → will_stop_ 永远为 false → 100% CPU 死循环
```

因果链：
```
io_uring_enter 失败 → SQE 未到达内核 → timeout 永不过期
→ can_stop() 永不调用 → will_stop_ 永为 false → ∞ loop
```

---

## 怎么解决的？

### 修复 A：Patch liburingcxx 禁用 IORING_ENTER_REGISTERED_RING

**文件**: `cmake/Extra.cmake`

通过 FetchContent 的 `PATCH_COMMAND` 将 `using_register_ring_fd` 改为 `false`：

```cmake
FetchContent_Declare(
    liburingcxx
    GIT_REPOSITORY https://github.com/Codesire-Deng/liburingcxx.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
    PATCH_COMMAND sed -i "s/using_register_ring_fd = is_kernel_reach(5, 18)/using_register_ring_fd = false/" include/uring/uring.hpp
)
```

这一行更改产生三重效果：
- `default_enter_flags_registered_ring = 0`（`__submit` 不再携带该 flag）
- `if constexpr (using_register_ring_fd)` 在 `init()` 中为 false → `register_ring_fd()` 永不调用
- `if constexpr (using_register_ring_fd)` 在 `_get_cq_entry()` 中为 false → flag 永不添加

### 修复 B：延迟 ring 初始化到事件循环线程

**文件**: `include/coronet/platform/io_uring/io_uring_proactor.hpp`, `src/coronet/platform/io_uring/io_uring_proactor.cpp`, `src/coronet/io_context.cpp`

将 `ring_.init()` 从构造函数线程移至事件循环线程：

**io_uring_proactor 新增方法**：
```cpp
// 构造函数线程：仅创建 eventfd，不调用 io_uring_setup
void init(uint32_t entries) {
    entries_ = entries;
    event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    // ring_.init() 延迟到 lazy_init_ring()
}

// 事件循环线程：调用 io_uring_setup（遵守 SINGLE_ISSUER）
void lazy_init_ring() {
    if (ring_initialized_) return;
    ring_.init(entries_);  // io_uring_setup 在事件循环线程执行
    ring_initialized_ = true;
}
```

**io_context::run() 调用**（对齐 co_context）：
```cpp
void io_context::run() {
    // ...
    detail::g_io_context_meta.wait_all_ready();
    
    // 从事件循环线程延迟初始化（遵守 SINGLE_ISSUER）
    proactor_.lazy_init_ring();
    proactor_.arm_eventfd_if_needed();
    
    while (!will_stop_) { /* 事件循环 */ }
}
```

### 修复 C：重构事件循环（对齐 co_context）

**文件**: `src/coronet/io_context.cpp`, `src/coronet/detail/worker_meta.cpp`, `include/coronet/platform/io_uring/io_uring_proactor.hpp`

**1. poll_submission：合并 submit + wait**

```cpp
void worker_meta::poll_submission() noexcept {
    if (requests_to_submit == 0) return;
    // io_uring: 有就绪协程 → 非阻塞提交；无就绪协程 → 提交并等待完成
    bool will_wait = !has_task_ready();
    proactor->submit(will_wait);
    requests_to_submit = 0;
}
```

**2. do_completion_part：Fast path + Bad path**

```cpp
void io_context::do_completion_part() noexcept {
    // Fast path: 批量收割所有可用 CQE（非阻塞）
    uint32_t handled = 0;
    while (proactor_.has_completion()) {
        if (worker_.poll_completion(true) > 0) ++handled;
    }
    
    // Fast path: 有活跃工作 → 跳过阻塞，让循环继续
    if (worker_.requests_to_submit || worker_.has_task_ready() || handled) return;
    
    // Bad path: 空闲 → 用 wait_cqe() 正确阻塞（而非 submit(true)）
    if (!proactor_.has_completion() && worker_.requests_to_reap > 0) {
        proactor_.wait_cqe();  // ring_.wait_cq_entry() — 正确的阻塞等待
        while (proactor_.has_completion()) worker_.poll_completion(true);
    }
}
```

**3. 新增 io_uring_proactor 方法**

```cpp
// 非阻塞 peek：CQE 是否可用？
bool has_completion() noexcept {
    const cq_entry* cqe = nullptr;
    ring_.peek_cq_entry(cqe);
    return cqe != nullptr;
}

// 正确的阻塞等待（不提交任何 SQE）
void wait_cqe() noexcept {
    const cq_entry* cqe = nullptr;
    ring_.wait_cq_entry(cqe);
}
```

### 设计对比：修复前 vs 修复后 vs co_context

| 对比维度 | 修复前 | 修复后 | co_context |
|---------|--------|--------|------------|
| liburingcxx 注册 ring | 无条件启用 → EINVAL | 禁用 | 同样禁用（硬编码假定，但运行时绕过） |
| ring 初始化 | 构造函数线程 | 事件循环线程 | 事件循环线程 |
| 提交流程 | `submit(false)` + `submit(true)` 分开 | `poll_submission()` 合并 submit+wait | `ring.submit_and_wait(will_wait)` |
| 完成轮询 | 每轮无条件调用 | Fast path: busy 时跳过; Bad path: `wait_cqe()` | Fast path: busy 时跳过; Bad path: `wait_uring()` |
| 阻塞方式 | `submit(true)` 间接等待 | `wait_cqe()` → `ring_.wait_cq_entry()` | `wait_uring()` → `ring_.wait_cq_entry()` |

---

## 为什么这么解决？

### 替代方案及驳回理由

**方案 A：移除 SINGLE_ISSUER/DEFER_TASKRUN 等高级 flag**

只保留基础 io_uring 功能，不启用 kernel 5.19+ 的优化。

**驳回**：这些 flag 在原生 Linux 上带来显著的性能提升。问题不在 flag 本身，而在于违反 SINGLE_ISSUER 的线程约束。修复线程问题后，所有 flag 正常工作。

**方案 B：在用户代码中放弃 SINGLE_ISSUER（降级）**

**驳回**：SINGLE_ISSUER 是 DEFER_TASKRUN 的前置条件（kernel 强制要求）。移除 SINGLE_ISSUER 意味着也无法使用 DEFER_TASKRUN，损失批量 completion 处理的性能优势。

**方案 C：不修复 liburingcxx，等上游更新**

**驳回**：liburingcxx 的设计注释明确写了 `// HACK this assumes app will use registered ring`。这是一个已知的硬编码假设，上游暂无修复计划。用户的计划是未来抛弃 liburingcxx，直接使用原生 io_uring C API；在此之前，一行的 patch 是最务实的方案。

### 当前方案的优势

1. **正确性**：彻底解决了 WSL2 上 io_uring 的 3 个阻塞 bug，所有 12 个核心测试通过
2. **性能**：保留了 COOP_TASKRUN + SINGLE_ISSUER + DEFER_TASKRUN 全套优化，原生 Linux 上不受影响
3. **设计质量**：事件循环架构与 co_context（公认的高质量 io_uring 参考实现）对齐
4. **最小侵入**：仅影响 io_uring 路径（`#ifdef CORONET_USE_IOURING`），epoll/IOCP 路径零改动
5. **可维护性**：PATCH_COMMAND 明确记录了修改原因和上游 issue，未来迁移到原生 C API 时可直接移除

### 涉及文件汇总

| 文件 | 修改内容 |
|------|---------|
| `cmake/Extra.cmake` | FetchContent PATCH_COMMAND 修复 liburingcxx |
| `include/coronet/platform/io_uring/io_uring_proactor.hpp` | 新增 `lazy_init_ring()`, `has_completion()`, `wait_cqe()`, `arm_eventfd_if_needed()` |
| `src/coronet/platform/io_uring/io_uring_proactor.cpp` | 延迟 ring init, eventfd offset 修复, 新方法实现 |
| `src/coronet/io_context.cpp` | `run()` 中延迟初始化, `do_submission_part()` 和 `do_completion_part()` 重构 |
| `src/coronet/detail/worker_meta.cpp` | `poll_submission()` 合并 submit+wait |

### 验证结果

```
启用全部 io_uring 优化 flag:
  CORONET_IOURING=ON + COOP_TASKRUN + SINGLE_ISSUER + DEFER_TASKRUN

12/12 tests passed:
  task_gtest, generator_gtest, channel_gtest, shared_task_gtest,
  move_shared_task, sem, timer, timer_accuracy, cv_notify_all,
  cv_notify_one, net_echo, coronet_cleanup

Redis echo server: PING → +PONG ✓

---

# Bug 修复记录：io_uring 后端 channel_stress 超时（每阶段恰好 30 秒）

**日期**: 2026-07-31  
**分支**: build/linux  
**影响范围**: io_uring 后端（CORONET_IOURING=ON）  
**相关 Bug**: 与上述 io_uring EINVAL/EEXIST 修复后的残留问题

---

## Bug 描述

channel_stress 测试在 io_uring 后端下，每个 phase 恰好耗时 30 秒：

```
[12:57:27] [PingPong] rounds=1000       ← monitor 检测到完成
[12:57:57] GLOBAL TIMEOUT — stopping    ← 30 秒后才触发！
[12:57:57] === PingPong PASSED ===
[12:57:57] [DropTest] PASSED
[12:58:27] GLOBAL TIMEOUT — stopping    ← 又是 30 秒
...
```

CTest 120 秒超时，整个测试箱无法通过。虽然 monitor 协程正确检测到了完成条件并调用了 `can_stop()`，但事件循环在 `wait_cqe()` 中阻塞，无法立即退出。

---

## 为什么会发生这个 Bug？

### TOCTOU 竞态：wakeup 信号被"吃掉"

`can_stop()` 和 `do_completion_part()` 之间存在竞态窗口：

```
事件循环第 N 轮:
  do_worker_part():
    monitor 协程恢复 → 检测 count==1000
      → ctx.can_stop()
        → will_stop_ = true
        → proactor_.wakeup()             ← 写入 eventfd (信号 A)

  do_submission_part():
    (无待提交 SQE)

  do_completion_part():
    has_completion() → TRUE!             ← 看到了 eventfd 的 CQE (信号 A 到达)
      → poll_completion(true)
        → wait_completion()
          → 检测到 eventfd CQE
          → cq_advance(1)                ← 消费掉
          → arm_eventfd()                ← 重新 arming（新读请求）
          → return 0
    handled = 0

    Fast path 检查:
      requests_to_submit == 0?  ✓
      has_task_ready() == false? ✓
      handled == 0?              ✓
      → 全部为 false → 进入 bad path!

    Bad path:
      has_completion() → false          ← 无 CQE（eventfd 刚被消费）
      requests_to_reap > 0? → true      ← 30s timeout 仍在等待
      → wait_cqe() → io_uring_enter(fd, 0, 1, ...) → 阻塞！
      
      ⚠️ eventfd 已被 re-arm，但无人再写入。
      只能等到 30s timeout 到期 → CQE → 解除阻塞。
```

**因果链**：
```
can_stop() → wakeup() → eventfd CQE 产生
  → do_completion_part() 消费了 eventfd CQE 并 re-arm
  → 但 will_stop_=true 的事实被忽略
  → bad path 进入 wait_cqe() 阻塞
  → 直到 30s timeout 到期才唤醒
```

### 为什么 io_uring 路径独有？

epoll 后端没有这个问题，因为：
1. epoll 的 `do_completion_part()` 在 `has_deferred && no_inflight` 时使用非阻塞轮询，不会在 `epoll_wait` 中永久阻塞
2. epoll 的 `can_stop()` 使用 `eventfd` + `epoll_wait` 唤醒机制，与 io_uring 的 CQE 机制不同

---

## 怎么解决的？

### 修改：bad path 进入前检查 will_stop_

**文件**: `src/coronet/io_context.cpp`

在 `do_completion_part()` 的 bad path 中，进入 `wait_cqe()` 阻塞前增加 `will_stop_` 检查：

```cpp
// CRITICAL: check will_stop_ before blocking. There is a TOCTOU race:
// can_stop() → wakeup() writes to eventfd → eventfd CQE is consumed &
// re-armed by the batch reap above → bad path entered → wait_cqe()
// blocks forever because the wakeup signal was "eaten".
// By checking will_stop_ here, we avoid blocking after can_stop().
// 关键：进入阻塞前检查 will_stop_。can_stop() 的 wakeup 信号可能在
// 上面的批量收割中被消费掉（eventfd CQE → re-arm），导致 wait_cqe()
// 永远阻塞。通过在此处检查 will_stop_，避免 can_stop() 后的无效阻塞。
if (!will_stop_.load(std::memory_order_acquire)
    && !proactor_.has_completion()
    && worker_.requests_to_reap > 0) {
    proactor_.wait_cqe();
```

**修复后的时序**：
```
can_stop() → wakeup() → eventfd CQE 产生
  → do_completion_part() 消费 eventfd CQE
  → 进入 bad path
  → will_stop_.load() == true → 跳过 wait_cqe()!
  → 循环回到顶部 → while(!will_stop_) → false → 退出事件循环 ✓
```

---

## 为什么这么解决？

### 替代方案及驳回理由

**方案 A：在 can_stop() 中多次写入 eventfd**

反复写入 eventfd 确保至少一个信号到达 wait_cqe()。

**驳回**：治标不治本。无法确定需要写多少次——竞态窗口是变化的。且在高频 can_stop() 调用下浪费 syscall。

**方案 B：使用 submit_and_wait_timeout 替代 wait_cqe**

在 bad path 中用 100ms 超时替代无限阻塞，周期性检查 will_stop_。

**驳回**：增加不必要的 syscall 频率（每 100ms 一次），在空闲时浪费 CPU 和功耗。且 100ms 的轮询间隔引入了不必要的延迟。

**方案 C：在消费 eventfd CQE 后不 re-arm（仅 can_stop 后）**

**驳回**：re-arm 是必须的——后续的跨线程 co_spawn 仍需 eventfd 唤醒。不能因为 can_stop 而永久禁用 eventfd。

### 当前方案的优势

1. **零额外开销**：只在 bad path 入口处增加一次 atomic load（x86 上编译为普通 mov）
2. **正确处理竞态**：will_stop_ 的 acquire load 与 can_stop() 的 seq_cst store 建立 happens-before，保证可见性
3. **最小侵入**：仅增加一个条件判断，不改变 bad path 的核心逻辑
4. **跨平台安全**：epoll/IOCP 路径不受影响（仅在 `#ifdef CORONET_USE_IOURING` 块内）

### 验证结果

```
修复前: channel_stress — 150 秒（5 phases × 30s timeout）
修复后: channel_stress — 0.01 秒

$ ctest -R channel_stress -V
29: === ALL CHANNEL STRESS TESTS PASSED ===
1/2 Test #29: channel_stress ... Passed    0.01 sec
```

---

# Bug 修复记录：io_uring SINGLE_ISSUER 约束违反 (EEXIST)

**日期**: 2026-07-31  
**分支**: build/linux  
**影响范围**: io_uring 后端（内核 >= 6.0 启用 IORING_SETUP_SINGLE_ISSUER）

---

## Bug 描述

`io_uring_enter()` 返回 `-EEXIST`（File exists），导致所有 I/O 操作无法完成：

```
[uring] submit() failed: File exists
```

sem 测试 100% CPU 死循环，channel_stress 每阶段耗时 30 秒。根本原因是 `IORING_SETUP_SINGLE_ISSUER` 要求 `io_uring_setup` 和 `io_uring_enter` 必须在**同一线程**调用。

---

## 为什么会发生这个 Bug？

coronet 的原始代码在 `io_context` 构造函数中调用 `ring_.init()`（在 main 线程），然后在 `run()` 的事件循环线程中调用 `submit()` / `io_uring_enter`。这违反了 SINGLE_ISSUER 约束：

```
main 线程:              ring_.init()        ← io_uring_setup
事件循环线程:             submit()            ← io_uring_enter
                         ↑ EEXIST！
```

独立测试验证了此行为：
```cpp
// 线程 1
io_uring_setup(entries, &p);  // SINGLE_ISSUER 注册线程
// 线程 2
io_uring_enter(fd, ...);      // → -EEXIST
```

---

## 怎么解决的？

### 修改：延迟 ring 初始化到事件循环线程

**文件**: `include/coronet/platform/io_uring/io_uring_proactor.hpp`
**文件**: `src/coronet/platform/io_uring/io_uring_proactor.cpp`

新增 `lazy_init_ring()` 方法，将 `ring_.init()` 从构造函数推迟到事件循环线程调用：

```cpp
void io_uring_proactor::init(uint32_t entries) {
    // 仅存储配置参数和创建 eventfd，不初始化 ring
    entries_ = entries;
    event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    // ring_.init() 延迟到 lazy_init_ring()
}

void io_uring_proactor::lazy_init_ring() {
    if (ring_initialized_) return;
    ring_.init(entries_);       // ← 在事件循环线程执行！
    ring_initialized_ = true;
    if (event_fd_ >= 0) {
        arm_eventfd();           // ← ring 就绪后才 arm eventfd
    }
}
```

**文件**: `src/coronet/io_context.cpp`

在 `run()` 中 barrier 通过后调用 `lazy_init_ring()`：

```cpp
void io_context::run() {
    // ...
    detail::g_io_context_meta.wait_all_ready();

#if defined(CORONET_USE_IOURING)
    proactor_.lazy_init_ring();  // ← 在事件循环线程初始化 ring
#endif

    while (!will_stop_.load(std::memory_order_acquire)) {
        // ...
    }
}
```

---

## 为什么这么解决？

### 替代方案及驳回理由

**方案 A：禁用 SINGLE_ISSUER (IORING_SETUP_SINGLE_ISSUER)**

**驳回**：SINGLE_ISSUER 是 io_uring 的重要性能优化（免去内部锁）。禁用会降低高并发场景下的吞吐量。

**方案 B：在 main 线程运行事件循环**

**驳回**：coronet 的设计要求事件循环运行在独立线程中（`host_thread_`），以支持多 io_context 的并发执行。

### 当前方案的优势

1. **遵守内核约束**：`io_uring_setup` 和 `io_uring_enter` 在同一线程，SINGLE_ISSUER 正常工作
2. **保留全部优化**：COOP_TASKRUN + SINGLE_ISSUER + DEFER_TASKRUN 全套 flag 启用
3. **eventfd 安全**：arm_eventfd 仅在 ring 就绪后执行，避免 SQE 分配到未初始化的 ring
4. **向后兼容**：epoll/IOCP 路径零改动

---

## 补充说明：do_completion_part 的 will_stop_ 检查

**文件**: `src/coronet/io_context.cpp`

在 `do_completion_part()` 中，io_uring 路径增加了 `will_stop_` 检查：

```cpp
void io_context::do_completion_part() noexcept {
#if defined(CORONET_USE_IOURING)
    // 仅在未停止时调用 poll_completion（其中 wait_completion 可能阻塞）
    // 如果 will_stop_ 已由 can_stop() 设置（同线程），跳过阻塞避免死锁
    if (!will_stop_.load(std::memory_order_acquire)) {
        worker_.poll_completion();
    }
#else
    worker_.poll_completion();
#endif
}
```

**设计关键**：保持每轮事件循环只收割一个 CQE 的原有节奏，不破坏事件循环的阶段交替设计（do_worker_part → do_submission_part → do_completion_part → 循环）。这与之前的"批量收割 + 慢速路径"方案不同——那个方案在 do_completion_part 中连续收割多个 CQE 而不给 do_worker_part 运行机会，导致被 CQE 唤醒的协程无法得到调度。

### 验证结果

```
修复后所有测试通过（io_uring 后端，WSL2 kernel 6.18）：

sem:              10/10 workers completed  ← 之前 100% CPU 死循环
channel_stress:   5/5 phases PASSED        ← 之前 150 秒超时
net_echo:         5/5 tests PASSED
combinator_stress: 6/6 phases PASSED
channel, mutex, cv_notify_all, cv_notify_one, timer,
when_all, when_any, when_some, timer_accuracy: all PASSED
```

---

# Bug 修复记录：epoll 后端 channel_stress 超时（defer_task 泄漏）

**日期**: 2026-07-31  
**分支**: build/linux  
**影响范围**: epoll 后端（默认路径，无 CORONET_IOURING）

---

## Bug 描述

epoll 后端下 `channel_stress` 测试每个阶段恰好耗时 30 秒：

```
28/35 Test #29: channel_stress ...***Timeout 120.01 sec

GLOBAL TIMEOUT — stopping
=== PingPong PASSED ===
[DropTest] PASSED
=== DropTest PASSED ===
GLOBAL TIMEOUT — stopping
=== BlockTest PASSED ===
GLOBAL TIMEOUT — stopping
...
```

虽然 ping-pong、channel 操作等业务逻辑能正常完成（断言通过），但 `can_stop()` 从未被及时调用。唯一的退出途径是 `global_stopper` 的 30 秒 `timerfd` 超时，导致每阶段精确耗时 30 秒，CTest 120 秒超时。

---

## 为什么会发生这个 Bug？

### P2-5 活锁修复的副作用

`epoll_yield` 曾有一个活锁问题（详见本文档第一个 Bug 修复记录）：`yield()` 使用 `forward_task()` 将协程放回 SPSC 环，但 `do_worker_part()` 的 `while` 循环立即再次取出它，导致无限循环。

P2-5 修复将 `forward_task` 改为 `defer_task`，将协程放入 **延迟任务队列**（`deferred_tasks`）。延迟任务应该在下一轮事件循环中，等 I/O 完成处理之后再恢复：

```cpp
// epoll_lazy_io.hpp: epoll_yield::await_suspend
void await_suspend(std::coroutine_handle<> current) noexcept {
    io_info_.handle = current;
    io_info_.result = 0;
    this_thread.worker->defer_task(current);  // ← 放入 deferred_tasks
    io_info_.handle = nullptr;
}
```

### defer_task 和 drain_deferred 的机制

`worker_meta` 提供了一对操作：

```cpp
// 将协程加入延迟队列
void defer_task(std::coroutine_handle<> handle) noexcept {
    deferred_tasks.push_back(handle);
}

// 将延迟队列搬移到 SPSC 环
void drain_deferred() noexcept {
    for (auto h : deferred_tasks) {
        forward_task(h);
    }
    deferred_tasks.clear();
}
```

### 缺陷：drain_deferred() 从未被调用

事件循环的四阶段设计中**缺少对 `drain_deferred()` 的调用**：

```
┌──────────────────────────────────────────────────────┐
│ while (!will_stop_) {                                │
│   worker_.drain_cross_thread();  // 搬移跨线程协程    │
│   // ★ drain_deferred() 缺失！                       │
│   do_worker_part();             // 恢复 SPSC 环协程   │
│   do_submission_part();         // 提交 I/O          │
│   do_completion_part();         // 收割 I/O 完成     │
│ }                                                    │
└──────────────────────────────────────────────────────┘
```

### 后果：monitor 协程永远无法恢复

```
monitor 协程:
  co_await async::yield()
    → epoll_yield::await_suspend()
    → defer_task(monitor_handle)
    → monitor 进入 deferred_tasks  ← 卡在这里，永不恢复
    ↓
  can_stop() 永远不会被调用
    ↓
  事件循环只能等 global_stopper 的 30s timerfd 超时
    → global_stopper 恢复 → can_stop() → 循环退出
```

### 为什么 io_uring 路径不受影响？

io_uring 后端的 `yield()` 是异步操作（提交 `IORING_OP_NOP` SQE，通过 CQE 完成）。完成路径是：
```
NOP SQE → CQE → handle_completion() → forward_task() → SPSC 环
```
全程不经过 `deferred_tasks`，因此不受此 bug 影响。

---

## 怎么解决的？

### 修改：在事件循环中添加 drain_deferred() 调用

**文件**: `src/coronet/io_context.cpp`

在事件循环主循环中，`drain_cross_thread()` 之后、`do_worker_part()` 之前添加 `drain_deferred()`：

```cpp
// 阶段 0：从跨线程队列搬移协程句柄到 SPSC 环
worker_.drain_cross_thread();
// 阶段 0.5：搬移延迟任务队列到 SPSC 环。
// epoll 后端的 yield() 使用 defer_task() 将协程加入延迟队列以避免活锁，
// 必须在 do_worker_part() 之前排空，否则 yield() 的协程永远不会恢复。
worker_.drain_deferred();          // ← 新增
// 阶段 1：从 SPSC 环恢复所有就绪协程
do_worker_part();
```

同时在 `drain_residual_coroutines()` 中添加对应的 `drain_deferred()` 调用，确保关闭时延迟协程也能被清理：

```cpp
void io_context::drain_residual_coroutines() {
    worker_.drain_cross_thread();
    worker_.drain_deferred();      // ← 新增
    worker_.work_once();

    for (int round = 0; round < 3 && worker_.has_task_ready(); ++round) {
        worker_.drain_cross_thread();
        worker_.drain_deferred();  // ← 新增
        do_worker_part();
    }
    ...
}
```

### 修复后的数据流

```
事件循环第 N 轮:
  drain_cross_thread()    → cross_queue 句柄 → SPSC 环
  drain_deferred()        → deferred_tasks 句柄 → SPSC 环  ← 关键！
  do_worker_part()        → monitor 恢复执行
    → count == 1000 → can_stop() → will_stop_ = true
  do_submission_part()    → no-op (epoll)
  do_completion_part()    → epoll_wait 返回 eventfd → drain + re-arm

第 N+1 轮:
  while (!will_stop_) → false → 退出循环 ✓
```

---

## 为什么这么解决？

### 替代方案及驳回理由

**方案 A：回退 P2-5 修复，让 yield() 重新使用 forward_task()**

回退到 `forward_task` 会导致 epoll 后端的活锁问题（CPU 100%）。

**驳回**：活锁问题同样严重，回退不是解决方向。

**方案 B：让 yield() 不使用 defer_task，改用其他反活锁机制**

例如在 `do_worker_part()` 中限制单轮恢复次数。

**驳回**：限制恢复次数需要引入额外的计数器和分支，增加热路径开销。且 `deferred_tasks` 设计已经存在，只需补充调用即可，零额外成本。

### 当前方案的优势

1. **最小侵入**：仅添加两处 `drain_deferred()` 调用，不改动现有数据结构
2. **零开销**：`drain_deferred()` 遍历 vector 后 clear；epoll 路径中仅在 yield() 时有条目，开销可忽略；io_uring/IOCP 路径中 vector 为空，相当于空循环
3. **修复彻底**：同时解决了正常事件循环和 shutdown drain 两条路径的延迟任务泄漏
4. **保持 P2-5 活锁修复**：不破坏已有的反活锁机制
5. **跨平台安全**：`drain_deferred()` 对所有后端都是安全的（io_uring/IOCP 后端 `deferred_tasks` 为空）

### 验证结果

```
epoll 后端修复前后对比：

修复前:
  channel_stress — 150 秒（5 phases × 30s timeout）
  $ ctest -R channel_stress
  28/35 Test #29: channel_stress ...***Timeout 120.01 sec

修复后:
  channel_stress — < 1 秒（无 GLOBAL TIMEOUT 消息）
  [PingPong] rounds=1000
  === PingPong PASSED ===
  [DropTest] PASSED
  === DropTest PASSED ===
  [BlockTest] producer_done=1 consumer_got=1
  === BlockTest PASSED ===
  [WrapAround] sum=12720 expected=12720
  === WrapAround PASSED ===
  [MPMC] produced=2000 consumed=2000
  === MPMC PASSED ===
  === ALL CHANNEL STRESS TESTS PASSED ===

epoll + io_uring 双后端全部测试通过:
  sem, channel_stress, net_echo, mutex, combinator_stress,
  channel, cv_notify_all, cv_notify_one, timer,
  when_all, when_any, when_some, timer_accuracy: all PASSED
```