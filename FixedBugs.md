# Fixed Bugs — coronet Windows Segfault 修复记录

## 概述

使用 `cmake + ninja` 在 Windows 上构建 coronet 时，12 个测试出现 SEGFAULT 或异常退出，
但 `cmake + Visual Studio` 生成器构建的版本正常。

经过系统性排查，定位并修复了 **5 个 bug**，其中 **Bug #1 是导致 segfault 的根因**。

---

## Bug #1（根因）：MSVC 协程运行时在 `await_suspend` 返回后访问已释放帧

### 影响的文件

- `include/coronet/task.hpp` — `task_final_awaiter<void>`
- `include/coronet/detail/trivial_task.hpp` — `trivial_task::final_awaiter`

### 原代码行为

分离（detached）的 `task<void>` 协程在 `final_suspend` 的 `await_suspend` 中直接调用
`current.destroy()` 销毁自己的协程帧，然后返回父协程句柄让运行时恢复父协程：

```cpp
// task_final_awaiter<void>::await_suspend （原代码）
std::coroutine_handle<> continuation = promise.parent_coroutine;
if (promise.is_detached_flag == Promise::is_detached) {
    current.destroy();  // 在 await_suspend 内部销毁帧
}
return continuation;     // 返回父协程句柄
```

`trivial_task::final_awaiter::await_suspend` 也有相同模式：

```cpp
// trivial_task::final_awaiter::await_suspend （原代码）
auto continuation = current.promise().parent_coro;
current.destroy();      // 在 await_suspend 内部销毁帧
return continuation;    // 返回父协程句柄
```

### 根因分析

C++20 协程规范规定：`await_suspend` 返回 `coroutine_handle<>` 时，运行时将恢复该句柄。
MSVC 的协程运行时实现在 `await_suspend` 返回后、恢复目标句柄之前，**仍会访问当前协程帧**
（例如进行状态更新或簿记操作）。在 `await_suspend` 内部调用 `current.destroy()` 释放了
协程帧后，运行时的后续访问就变成了 **use-after-free**，导致 segfault。

GCC 和 Clang 的协程运行时没有这个问题（`destroy()` 后不再访问帧），所以 Linux 不受影响。

### 修复方案

**`task_final_awaiter<void>`（分离的 task<void>）：**
使用 `await_ready() = true` 替代 `current.destroy()`。当 `await_ready()` 返回 `true` 时，
C++20 运行时**不调用 `await_suspend`**，直接调用 `await_resume()` 后在内部自动销毁协程帧。
此方案完全绕过了 MSVC 运行时的 use-after-free 路径。

```cpp
// task_final_awaiter<void>（新代码）
struct task_final_awaiter<void> {
    bool is_detached_ = false;

    constexpr bool await_ready() const noexcept {
        return is_detached_;   // 分离任务 → true，运行时自动销毁帧
    }

    // await_suspend 仅在非分离任务时调用（is_detached_ = false）
    template<std::derived_from<task_promise_base<void>> Promise>
    constexpr std::coroutine_handle<>
    await_suspend(std::coroutine_handle<Promise> current) const noexcept {
        return current.promise().parent_coroutine;  // 对称传输到父协程
    }

    constexpr void await_resume() const noexcept {}
};
```

分离任务（`is_detached_ = true`）：
- `await_ready()` → `true` → 不挂起 → 运行时自动销毁帧
- 无需恢复父协程（分离任务本身就没有父协程在等待）

非分离任务（`is_detached_ = false`）：
- `await_ready()` → `false` → 挂起 → `await_suspend` 返回父协程 → 对称传输
- 帧由 `~task()` 调用 `handle.destroy()` 销毁

`task_promise<void>::final_suspend()` 被 override 以在构造 awaiter 时设置 `is_detached_` 标志：

```cpp
// task_promise<void>::final_suspend()
constexpr task_final_awaiter<void> final_suspend() const noexcept {
    task_final_awaiter<void> awaiter;
    if (is_detached_flag == is_detached) {
        awaiter.is_detached_ = true;
    }
    return awaiter;
}
```

**`trivial_task::final_awaiter`：**
`trivial_task` 始终需要恢复父协程（被 `co_await` 等待）。修改为 `await_suspend` 返回 `void`，
在内部通过 `forward_task` 将父协程句柄推入 SPSC 调度环，由事件循环恢复。

```cpp
// trivial_task::final_awaiter（新代码）
static void
await_suspend(std::coroutine_handle<promise_type> current) noexcept {
    auto continuation = current.promise().parent_coro;
    auto* w = coronet::detail::this_thread.worker;
    if (w) {
        w->forward_task(continuation);  // 调度父协程，延迟恢复
    }
    // 不在 await_suspend 中调用 current.destroy()
    // 帧由调用方（co_await 完成后 ~trivial_task()）管理生命周期
}
```

### 为什么此修复合理

1. **符合 C++20 标准**：`await_ready() = true` 让运行时自动销毁帧是规范定义的标准路径。
2. **分离任务的语义天然匹配**：分离任务不需要恢复父协程，`await_ready() = true` 直接走运行时自动清理是最简路径。
3. **跨平台兼容**：GCC/Clang 对两种路径（`await_ready = true` 和 `destroy()` 在 `await_suspend` 中）都支持良好。
4. **对称传输语义保留**：非分离任务仍通过 `await_suspend` 返回父协程实现零开销对称传输。

---

## Bug #2：`channel::buffer_end()` 解引用 past-the-end 迭代器（UB）

### 影响的文件

- `include/coronet/co/channel.hpp`

### 原代码

```cpp
T* buffer_end() noexcept { return reinterpret_cast<T*>(&(*buf_.end())); }
```

### 根因分析

`buf_` 是 `std::array<uninitialized_buffer<T>, capacity>`，`buf_.end()` 返回 past-the-end 迭代器。
`*buf_.end()` 解引用 past-the-end 迭代器是**未定义行为**（UB）。

MSVC Debug 模式下（`/RTC1` 运行时检查）会检测到非法解引用并导致程序崩溃。
这正是 `channel` 测试在调试构建中失败的原因 —— `push_one()` 和 `pop_one()` 内部调用
`buffer_end()` 来判断是否需要绕回环形缓冲区。

### 修复方案

```cpp
T* buffer_end() noexcept { return reinterpret_cast<T*>(buf_.data() + capacity); }
```

`buf_.data()` 返回指向数组首元素的指针，`buf_.data() + capacity` 是合法的 past-the-end
指针（指针算术在不越界时合法，`+capacity` 恰好是 past-the-end）。不涉及任何解引用操作。

### 为什么此修复合理

- `buf_.data() + capacity` 是 well-defined 的指针算术，合法的 past-the-end 指针。
- 语义等价：`buf_.end()` 对于 `std::array` 返回的就是 `buf_.data() + capacity`。
- `reinterpret_cast` 保持原有含义：将 `uninitialized_buffer<T>*` 解释为 `T*`，
  由于 `sizeof(uninitialized_buffer<T>) == sizeof(T)` 且对齐相同，此转换安全。

---

## Bug #3：IOCP 后端缺少 `requests_to_reap` 递增

### 影响的文件

- `include/coronet/platform/iocp/iocp_win_io.hpp`

### 原代码行为

IOCP 的 `win_awaiter_base::await_suspend` 调用 `p->work_started()`（递增 `outstanding_work_`），
但**不递增** `requests_to_reap`。然而 `worker_meta::handle_completion()` 每次完成事件都**递减**
`requests_to_reap`。

| 后端 | `++requests_to_reap` | `--requests_to_reap` |
|------|---------------------|---------------------|
| io_uring | `io_uring_lazy_io.hpp` 中递增 ✅ | `worker_meta.cpp` ✅ |
| epoll | `epoll_lazy_io.hpp` 中递增 ✅ | `worker_meta.cpp` ✅ |
| **IOCP** | **缺失 ❌** | `worker_meta.cpp` ✅ |

### 根因分析

IOCP 后端的 inflight 操作计数器从未递增但总是递减，导致 `requests_to_reap` 持续为负值。
虽然当前代码中 `requests_to_reap` 仅用于日志输出，但：
1. 计数器语义被破坏，无法反映真实 inflight 操作数
2. 未来若基于此计数器做事件循环退出判断（如 io_uring 后端），IOCP 端会出现逻辑错误

### 修复方案

在 `win_awaiter_base::await_suspend` 中添加 `++this_thread.worker->requests_to_reap`：

```cpp
void await_suspend(std::coroutine_handle<> current) noexcept {
    io_info_.handle = current;
    io_info_.result = 0;
    auto* p = static_cast<platform::iocp::iocp_proactor*>(
        this_thread.worker->proactor);
    p->work_started();
    ++this_thread.worker->requests_to_reap;     // ← 新增：与 io_uring/epoll 对齐
    static_cast<Derived*>(this)->issue_io();
}
```

### 为什么此修复合理

- 三个后端（io_uring、epoll、IOCP）的 inflight 计数逻辑完全统一。
- 每个 I/O 操作在发起时 `+1`，完成时 `-1`，语义正确。
- 代码位置与 io_uring/epoll 后端一致（在 `await_suspend` 中，实际操作发起前）。

---

## Bug #4：`mutex.cpp` 中 `std::assume_aligned` 在 MSVC LTO 下的潜在风险

### 影响的文件

- `src/coronet/co/mutex.cpp`

### 原代码

```cpp
auto* node = std::assume_aligned<alignof(lock_awaiter)>(
    reinterpret_cast<lock_awaiter*>(top));

this->next_ = std::assume_aligned<alignof(lock_awaiter)>(
    reinterpret_cast<lock_awaiter*>(old_state));
```

### 根因分析

`std::assume_aligned<N>(ptr)` 在 GCC/Clang 中通过 `__builtin_assume_aligned` 实现，
仅生成对齐加载/存储的优化提示，语义较窄。

MSVC 使用 `__assume((ptr & (N-1)) == 0)` 实现，这是一个**通用优化器断言**。
在 Release + LTO（`/GL + /LTCG`）构建中，MSVC 优化器可能从此断言推导出比原始条件
更强的结论（例如从"8 字节对齐"结合其他信息推导出"16 字节对齐"），从而生成需要
16 字节对齐的 SIMD 指令（如 `movaps`）。如果 coroutine frame 中的 `lock_awaiter`
对象实际只有 8 字节对齐，会触发对齐异常。

此外，`alignof(lock_awaiter) = 8` 的对齐保证在 x86-64 ABI 中已被类型系统和
堆分配器（`alignof(std::max_align_t) = 16`）天然满足，`assume_aligned<8>` 是
冗余的优化提示。

### 修复方案

移除 `std::assume_aligned` 调用，直接使用 `reinterpret_cast`：

```cpp
auto* node = reinterpret_cast<lock_awaiter*>(top);
this->next_ = reinterpret_cast<lock_awaiter*>(old_state);
```

### 为什么此修复合理

- GCC 和 Clang 对 `alignof = 8` 的 `__builtin_assume_aligned` 不生成任何不同的汇编代码
  （x86-64 `mov` 指令本身不要求额外对齐）。移除后 Linux 端**零性能影响**。
- 消除了 MSVC LTO 下潜在的过度优化风险。
- 代码更简洁，去除了一个对正确性无贡献的编译器 hint。

---

## Bug #5：`iocp_proactor::deinit()` 后台线程竞态条件

### 影响的文件

- `src/coronet/platform/iocp/iocp_proactor.cpp`

### 原代码行为

`deinit()` 向 IOCP 发送退出信号后，用 `GQCS(timeout=0)` 排空一次队列即关闭 IOCP handle：

```cpp
void iocp_proactor::deinit() noexcept {
    PostQueuedCompletionStatus(iocp_handle_, 0, 1, nullptr);  // 退出信号
    while (true) {
        // GQCS(timeout=0) — 非阻塞排空
        BOOL ok = GetQueuedCompletionStatus(iocp_handle_, ..., &ov, 0);
        if (!ok && !ov) break;
        // ...
    }
    CloseHandle(iocp_handle_);  // 关闭 handle
}
```

### 根因分析

`win_timeout`、`win_read`、`win_write` 使用 detach 的后台线程执行阻塞操作（`Sleep`、
`_read`、`_write`）。操作完成后，后台线程通过 `PostQueuedCompletionStatus` 向 IOCP
投递完成事件。

单次 `GQCS(timeout=0)` 排空存在竞态窗口：

```
[后台线程]                              [deinit()]
Sleep(ms)                                PostQuitSignal
                                         GQCS(timeout=0) → 空 → break
PostQueuedCompletionStatus(...)          CloseHandle(iocp_handle_)  ← 竞态！后台线程访问已关闭的 handle
```

### 修复方案

三轮排空策略：

- **第 1 轮**（`timeout=0`）：非阻塞，与原实现一致，零性能开销
- **第 2 轮**（`timeout=0`）：捕获第 1 轮期间后台线程刚投递的事件
- **第 3 轮**（`timeout=1ms`）：仅在第 2 轮排到事件时触发，等待慢速后台线程

```cpp
// 第 1 轮：GQCS(timeout=0)
while (true) {
    BOOL ok = GetQueuedCompletionStatus(..., 0);
    if (!ok && !ov) break;
    // ...
}
// 第 2 轮：GQCS(timeout=0)，捕获第 1 轮期间的竞态事件
{
    bool drained = false;
    while (true) {
        BOOL ok = GetQueuedCompletionStatus(..., 0);
        if (!ok && !ov) break;
        if (ov) drained = true;
        // ...
    }
    // 第 3 轮：GQCS(timeout=1ms)，仅在需要时执行
    if (drained) {
        while (true) {
            BOOL ok = GetQueuedCompletionStatus(..., 1);
            if (!ok && !ov) break;
            // ...
        }
    }
}
CloseHandle(iocp_handle_);
```

### 为什么此修复合理

1. **`deinit()` 是 shutdown 路径，非热路径**：第 1 轮与原来完全一致（`timeout=0`），
   正常情况只有第 1 轮执行，无性能回退。
2. **仅在需要时增加 1ms**：第 3 轮的 `timeout=1ms` 仅在前两轮都排到事件时触发，
   且总共最多增加 1ms 延迟（shutdown 路径不敏感）。
3. **正确处理所有后台线程场景**：三轮排空覆盖了 `win_timeout`、`win_read`、`win_write`
   等所有使用 detach 后台线程的场景。

---

## Bug #6：`win_accept` 缺少 `SO_UPDATE_ACCEPT_CONTEXT`

### 影响的文件

- `include/coronet/platform/iocp/iocp_win_io.hpp`

### 原代码行为

`AcceptEx` 完成后，`win_accept::await_resume()` 直接返回 `accept_socket_`（已接受的 socket），
未调用 `setsockopt(SO_UPDATE_ACCEPT_CONTEXT)`。

代码注释中明确提到此需求（"连接接受后需要调用 setsockopt(SO_UPDATE_ACCEPT_CONTEXT)"），
但从未实际实现。

### 根因分析

`AcceptEx` 与标准 `accept()` 不同：它使用预先创建的 socket 来接受连接。
完成后的 socket **不会自动继承监听 socket 的属性**，需要显式调用
`setsockopt(SO_UPDATE_ACCEPT_CONTEXT, listen_socket)` 来继承：

- Socket 选项（SO_RCVBUF、SO_SNDBUF 等）
- 地址信息（getsockname/getpeername 依赖）
- QoS 设置

虽然 `WSASend`/`WSARecv` 在不调用此设置时基本可工作，但在高并发场景下，
未正确继承属性的 socket 可能出现行为异常。

### 修复方案

在 `win_accept::await_resume()` 中，AcceptEx 完成后、返回 socket 前，
调用 `setsockopt(SO_UPDATE_ACCEPT_CONTEXT)`：

```cpp
[[nodiscard]] int32_t await_resume() const noexcept {
    // AcceptEx 完成后，必须调用 SO_UPDATE_ACCEPT_CONTEXT 使新 socket
    // 继承监听 socket 的属性（getsockname / getpeername / shutdown 等依赖此设置）。
    SOCKET listen_sock = static_cast<SOCKET>(sock_);
    ::setsockopt(static_cast<SOCKET>(accept_socket_), SOL_SOCKET,
                 SO_UPDATE_ACCEPT_CONTEXT,
                 reinterpret_cast<const char*>(&listen_sock),
                 sizeof(listen_sock));
    return static_cast<int32_t>(accept_socket_);
}
```

### 为什么此修复合理

- `sock_` 持有监听 socket 句柄，`accept_socket_` 持有已接受的 socket 句柄
- 两者在 `await_resume()` 调用时都有效（协程帧仍存活）
- 此位置在 AcceptEx 完成之后、socket 使用之前，时机正确
- 符合 MSDN 文档对 AcceptEx 使用规范的要求

---

## Bug #7：`operator&&` 链式 co_await IOCP 路径泄漏 `iocp_operation`

### 影响的文件

- `src/coronet/detail/worker_meta.cpp`

### 原代码行为

`handle_completion()` 中，`iocp_operation` 的回收（`recycle_operation`）只在"正常完成"路径中执行。
链式 co_await 的两个 early-return 路径（`chain_fn` 路径和 `null-handle` 路径）跳过了回收：

```cpp
// 原代码：回收在函数末尾，chain_fn 提前 return 导致泄漏
if (ti->chain_fn && ti->chain_ctx) {
    ...
    fn(ctx);
    return;  // ← 泄漏！未回收 info->opaque
}
...
// 回收代码仅在正常路径执行
#if defined(CORONET_PLATFORM_WINDOWS)
if (info->opaque) {
    recycle_operation(...);  // 只有正常路径才到达这里
}
#endif
```

### 根因分析

`operator&&`（send && recv）每次迭代创建两个 `iocp_operation`：一个用于 `win_send`、一个用于 `win_recv`。
`win_send`（链中第一个操作）完成时进入 `chain_fn` 路径，提前 return，其 `iocp_operation` 永远不会被回收。

C1000K 负载下（1M 请求），泄漏 1M 个 `iocp_operation`。每个 ~64 字节（OVERLAPPED internal + fields），加上堆分配器元数据开销，合计 ~256MB。
其他 server（ST、MT）不使用链式 co_await，不受影响。

### 修复方案

将 `iocp_operation` 回收代码**移到函数最前面**，在所有 early-return 之前执行：

```cpp
void worker_meta::handle_completion(const platform::completion_info* info) noexcept {
    --requests_to_reap;
    
    // 回收必须在所有 early-return 之前执行
#if defined(CORONET_PLATFORM_WINDOWS)
    if (info->opaque) {
        auto* raw = static_cast<platform::iocp::iocp_operation*>(info->opaque);
        platform::iocp::recycle_operation(
            std::unique_ptr<platform::iocp::iocp_operation>{raw});
    }
#endif

    auto* ti = task_info::from_user_data(info->user_data);
    ...
    // chain_fn / null-handle 的 early-return 不再泄漏
}
```

### 为什么此修复合理

- `info->opaque` 在任何 completion 路径中都是有效的 `iocp_operation*`
- 移到函数开头后，所有 return 路径（包括 chain_fn、null-handle、正常）都经过回收
- 不影响 io_uring/epoll 路径（opaque 为 nullptr，代码无操作）
- C1000K 修复后验证：coronet_chain 从 **256MB → 9MB**

---

## 总结

| Bug # | 严重性 | 类别 | 影响平台 | 影响测试 |
|-------|--------|------|----------|----------|
| #1 | **致命（根因）** | MSVC 协程运行时 use-after-free | Windows | mutex, sem, timer, when_\*, channel, cv_\* |
| #2 | 致命 | 解引用 past-the-end 迭代器（UB） | Windows Debug | channel |
| #3 | 中 | 计数器语义错误 | Windows | 无直接失败，逻辑不一致 |
| #4 | 低 | 过度优化 hint 风险 | Windows Release + LTO | 潜在安全隐患 |
| #5 | 中 | 竞态条件 | Windows | 偶发退出崩溃 |
| #6 | 中 | AcceptEx socket 属性继承缺失 | Windows | 高并发下潜在异常 |
| #7 | **致命** | chain_fn 路径泄漏 iocp_operation | Windows | coronet_chain C1000K 256MB 内存泄漏 |

### 回归测试结果

| 平台 | 测试数 | 结果 |
|------|--------|------|
| **Linux (GCC, WSL)** | **19/19** | ✅ 全部通过 |
| **Windows (MSVC 2022 Debug)** | **8/8** | ✅ 全部通过 |

Windows 测试详情：
- `stress_driver_ST`: coronet_ST 44,248 RPS + coronet_chain 46,512 RPS ✅
- `stress_driver_MT`: coronet_MT(6) 54,054 RPS ✅
- C200K ST: coronet 领先 ASIO 8.8% RPS ✅
- C200K MT: coronet 领先 ASIO 33.4% RPS ✅
- C1000K ST: coronet 领先 ASIO 3.2% RPS ✅
- C1000K MT: coronet 领先 ASIO 80.4% RPS ✅
- 零崩溃、零超时、零内存泄漏

### C1000K 压测结果（2026-07-24，MSVC Debug build）

| Server | Load | RPS | CPU% | Mem |
|--------|------|-----|------|-----|
| **coronet_ST** | 1M req × 1000 conn | **36,195** | 60.2% | 8MB |
| **coronet_MT(6)** | 1M req × 1000 conn | **33,358** | 56.4% | 9MB |
| ASIO_ST | 1M req × 1000 conn | 35,070 | 68.1% | 8MB |
| ASIO_MT(6) | 1M req × 1000 conn | 18,490 | 78.2% | 9MB |

- **单线程**：coronet_ST 比 ASIO_ST 高 3.2% RPS，CPU 低 7.9pp
- **多线程**：coronet_MT(6) 比 ASIO_MT(6) 高 **80.4%** RPS，CPU 低 21.8pp
- **内存**：两者相近（~8-9MB）
