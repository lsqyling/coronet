# coronet Code Review 报告

> 最新更新: 2026-07-27

## 目录

- [2026-07-27: TLS 模块审查](#2026-07-27-tls-模块审查)
  - [TLS 代码审查](#tls-代码审查)
  - [OpenSSL 构建系统方案](#openssl-构建系统方案)
  - [TLS 修复优先级](#tls-修复优先级)
- [2026-07-26: 全面 Code Review + 修复执行](#2026-07-26-全面-code-review--修复执行)
  - [架构总评](#架构总评)
  - [问题清单（按严重程度分级）](#问题清单按严重程度分级)
  - [API 设计审查](#api-设计审查)
  - [修复优先级建议（2026-07-26）](#修复优先级建议2026-07-26)
  - [总结](#总结)
- [2026-07-25 ~ 2026-07-26: 核心模块审查](#2026-07-25--2026-07-26-核心模块审查)
  - [task 模块深度审查](#task-模块深度审查)
  - [io_context 调度中心审查](#io_context-调度中心审查)
  - [net/ 模块审查](#net-模块审查)
  - [第一阶段：全量 Code Review（缓存行 / C++20 现代化 / 跨平台）](#第一阶段全量-code-review缓存行--c20-现代化--跨平台)
  - [验证结果总览](#验证结果总览)
- [关键设计决策回顾](#关键设计决策回顾)
- [附录：改动统计与文件清单](#附录改动统计与文件清单)

---

## 2026-07-27: TLS 模块审查

### TLS 代码审查

**审查范围**: `include/coronet/net/tls/` (3 文件) + `cmake/Extra.cmake` + `cmake/Option.cmake`

#### P1-1: `tls_context` 移动构造/赋值不转移 `alpn_protocols_` — use-after-free

**文件**: `include/coronet/net/tls/tls_context.hpp` L90-103

**问题**: 移动构造和移动赋值只转移 `ctx_` 和 `mode_`，**不转移 `alpn_protocols_`**：

```cpp
// 移动构造 — 缺少 alpn_protocols_ 转移
tls_context(tls_context&& other) noexcept
    : ctx_(other.ctx_), mode_(other.mode_) {
    other.ctx_ = nullptr;
    // BUG: alpn_protocols_ 未转移！
}

// 移动赋值 — 同样缺少
tls_context& operator=(tls_context&& other) noexcept {
    if (this != &other) {
        if (ctx_) SSL_CTX_free(ctx_);
        ctx_ = other.ctx_;
        mode_ = other.mode_;
        other.ctx_ = nullptr;
        // BUG: alpn_protocols_ 未转移！
    }
    return *this;
}
```

**后果**: 如果在 `set_alpn()` 后移动 `tls_context`：
1. 移动后 `this->ctx_` 指向 `other` 的 SSL_CTX（其 `app_data` 指向 `other.alpn_protocols_`）
2. `other` 析构时 `alpn_protocols_` 被 `unique_ptr` 释放
3. `this->ctx_->app_data` 变成悬垂指针 → **use-after-free**

**修复**: 转移 `alpn_protocols_`：

```cpp
tls_context(tls_context&& other) noexcept
    : ctx_(other.ctx_)
    , mode_(other.mode_)
    , alpn_protocols_(std::move(other.alpn_protocols_)) {
    other.ctx_ = nullptr;
}
```

#### P1-2: `shutdown_write()` 重试逻辑不完整

**文件**: `include/coronet/net/tls/tls_socket.hpp` L302-320

```cpp
int ret = SSL_shutdown(ssl_);
if (ret < 0) {
    int err = SSL_get_error(ssl_, ret);
    if (err == SSL_ERROR_WANT_WRITE) {
        co_await flush_wbio();
        (void)SSL_shutdown(ssl_);  // 仅重试一次，不处理 WANT_READ
    }
}
```

**问题**: 只重试一次，不处理 `SSL_ERROR_WANT_READ`，不循环。

**修复**: 改为循环（类似 `do_handshake()`）：

```cpp
task<void> shutdown_write() {
    if (!handshake_done_ || closed_ || !ssl_) co_return;
    while (true) {
        int ret = SSL_shutdown(ssl_);
        if (ret >= 0) { co_await flush_wbio(); co_return; }
        int err = SSL_get_error(ssl_, ret);
        if (err == SSL_ERROR_WANT_READ) {
            co_await flush_wbio();
            int raw_n = co_await tcp_.recv(raw_buf_);
            if (raw_n <= 0) co_return;
            BIO_write(SSL_get_rbio(ssl_), raw_buf_, raw_n);
        } else if (err == SSL_ERROR_WANT_WRITE) {
            co_await flush_wbio();
        } else {
            co_await flush_wbio();
            co_return;  // 致命错误，放弃
        }
    }
}
```

#### P2-1: `close()` 不执行完整的双向 SSL_shutdown

**文件**: `include/coronet/net/tls/tls_socket.hpp` L279-299

`(void)SSL_shutdown(ssl_)` 只调用一次，不等待对端 close_notify。这可能导致对端收到 RST 而非干净的关闭。建议添加 `close_graceful()` 方法做完整双向关闭。

#### P2-2: 缺少安全加固选项

**文件**: `include/coronet/net/tls/tls_context.hpp` L250-278 (`init_ctx`)

缺少以下安全选项：
- `SSL_OP_NO_COMPRESSION` — 防止 CRIME 攻击
- `SSL_OP_NO_RENEGOTIATION` — 防止重协商攻击
- `SSL_CTX_set_cipher_list` — 限制弱密码套件

建议在 `init_ctx` 中添加：

```cpp
SSL_CTX_set_options(ctx_, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
```

#### P2-3: `tls_acceptor` 裸指针生命周期无强制

**文件**: `include/coronet/net/tls/tls_acceptor.hpp` L93

`const tls_context* ctx_` 是裸指针，如果 `tls_context` 先于 `tls_acceptor` 析构，use-after-free。建议改为 `std::shared_ptr<const tls_context>` 或在文档中用 `[[nodiscard]]` + 析构断言强制。

#### P3-1: `set_verify_peer` 变量名遮蔽

```cpp
void set_verify_peer(bool on) {
    int mode = on ? ...;  // 遮蔽成员 mode_
```

建议改为 `int verify_mode`。

#### P3-2: `native_handle()` 返回 SSL* 而非 TCP fd

transport concept 的 `native_handle()` 应返回平台原生句柄。当前返回 SSL* 指针，可能让用户困惑。建议返回 `tcp_.native_handle()` 或文档说明差异。

---

### OpenSSL 构建系统方案

#### 现状

```cmake
# Extra.cmake — 硬依赖，找不到就 FATAL_ERROR
find_package(OpenSSL REQUIRED)
```

#### 方案对比

| | Git Submodule | **FetchContent (推荐)** |
|---|---|---|
| 仓库体积 | +50MB submodule | 零 |
| 用户操作 | 需 `git submodule update --init` | 全自动 |
| 版本控制 | submodule pin | `GIT_TAG` pin |
| 离线构建 | 支持 | 需首次联网 |
| 构建复杂度 | OpenSSL CMake 构建复杂 | 同左，但仅 fallback 时触发 |
| Linux | `find_package` 先成功，不触发 | 同左 |

#### 推荐方案：FetchContent fallback

**推荐理由**：
1. Linux 上 `find_package` 几乎总是成功，fallback 极少触发
2. Git submodule 会让仓库变重，且需要用户手动 init
3. FetchContent 全自动，首次联网后可离线（CMake 缓存）
4. 不污染 git 历史

#### 实现方案

```cmake
# ---- OpenSSL (for TLS support) ----
if(CORONET_WITH_TLS)
    # 1. 优先使用系统已安装的 OpenSSL
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        target_link_libraries(coronet PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(coronet PUBLIC CORONET_HAS_TLS)
        message(STATUS "coronet: TLS enabled (system OpenSSL ${OpenSSL_VERSION})")
    else()
        # 2. Fallback: FetchContent 下载并编译 OpenSSL
        message(STATUS "coronet: System OpenSSL not found, fetching from source...")

        include(FetchContent)
        FetchContent_Declare(
            openssl
            GIT_REPOSITORY https://github.com/openssl/openssl.git
            GIT_TAG        openssl-3.4.0
            GIT_SHALLOW    TRUE
        )

        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(WITH_APPS OFF CACHE BOOL "" FORCE)
        set(WITH_TESTS OFF CACHE BOOL "" FORCE)

        FetchContent_MakeAvailable(openssl)

        target_link_libraries(coronet PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(coronet PUBLIC CORONET_HAS_TLS)
        message(STATUS "coronet: TLS enabled (bundled OpenSSL from FetchContent)")
    endif()
endif()
```

**注意事项**：

1. **Perl 依赖**: OpenSSL 3.x 构建需要 Perl。在 `FetchContent_MakeAvailable` 前检测：

```cmake
find_program(PERL_EXECUTABLE perl REQUIRED)
if(NOT PERL_EXECUTABLE)
    message(FATAL_ERROR "coronet: Building OpenSSL from source requires Perl.")
endif()
```

2. **Windows 替代方案**: 推荐 vcpkg：

```
vcpkg install openssl
cmake -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake ..
```

3. **构建缓存**: FetchContent 下载后缓存在 `build/_deps/`，重复构建不会重新下载。

---

### TLS 修复优先级

| 优先级 | 问题 | 复杂度 |
|--------|------|--------|
| **P1-1** | tls_context 移动不转移 alpn_protocols_ | 低（加一行） |
| **P1-2** | shutdown_write 重试逻辑 | 中 |
| **P2-1** | close() 双向 shutdown | 低（新方法） |
| **P2-2** | 安全加固选项 | 低 |
| **P2-3** | tls_acceptor 生命周期 | 低 |
| **CMake** | FetchContent fallback | 中 |

---

## 2026-07-26: 全面 Code Review + 修复执行

**审查范围**: 核心库 (`include/coronet/`)、实现 (`src/coronet/`)、构建系统 (`CMakeLists.txt`, `cmake/`)

**重点**: 架构设计、C++20 协程执行流、并发安全、内存安全、性能、API 易用性

### 架构总评

#### 优势

1. **静态多态设计优秀** — Proactor 通过 `#ifdef` 编译期选择为值成员，零 vtable 开销。`proactor_selector.hpp` 作为单一真相源，消除了上层 `#ifdef` 散布。
2. **CRTP 层次清晰** — `socket_base<Derived>` → `tcp_socket`/`udp_socket`，链式方法返回 `Derived&`，符合零开销抽象原则。
3. **缓存行隔离意识强** — `worker_meta` 的 hot/cold 分离、`spinlock` 的 `alignas(64)`、`io_context::will_stop_` 的缓存行隔离，均体现了对 false sharing 的深入理解。
4. **IOCP 操作回收模式** — per-thread `op_free_list` 复用 `iocp_operation`，避免高频 I/O 下的堆分配，遵循 ASIO 经典优化。
5. **协程安全约定** — "协程函数不用协程 lambda"、"union 成员用 `construct_at`"、"null handle 返回 false 而非 true" 等约定已落实在代码中。

#### 架构建议（非 bug）

- **`async_io.hpp` 的 `timeout(auto dur)` 是缩写函数模板** — 每个不同的 duration 类型会生成新的实例化。建议用 `std::chrono::duration` concept 约束，减少编译时间和代码膨胀。
- **`chained_awaiter` 的 `operator&&` 返回值依赖编译期路径** — io_uring 返回 `uring_link_io<B>`（仅存指针），epoll/IOCP 返回 `chained_awaiter<A,B>`（move 两个 awaiter）。两条路径的 ABI 不一致，但符合各平台最优设计，可接受。

### 问题清单（按严重程度分级）

#### P0-1: `trivial_task` 协程帧泄漏

**文件**: `include/coronet/detail/trivial_task.hpp`

**调用路径**: `condition_variable::wait(mutex, predicate)` → 返回 `trivial_task` → 用户 `co_await` → 帧泄漏

**问题**: `trivial_task` 类没有析构函数来销毁协程帧。`final_awaiter::await_suspend` 返回 `void`（让运行时将帧留在 final suspend 点），注释说 "Current frame lifetime managed by the caller"，但调用方从未销毁帧：

```cpp
class trivial_task {
    std::coroutine_handle<promise_type> handle;  // 裸句柄，析构时丢失
    // 没有析构函数！
};
```

每次调用 `cv.wait(mtx, pred)` 都会泄漏一个协程帧（含 promise + 局部变量 + 状态机，通常 200-500 字节）。在高频条件变量场景下（如 channel 的 acquire/release），内存会持续增长。

**修复方案**:

```cpp
~trivial_task() {
    if (handle) handle.destroy();
}
```

由于 `final_awaiter::await_suspend` 返回 void 将帧留在 final suspend 点，此时 `handle.done() == true`，调用 `destroy()` 是安全的。需要确保 `await_resume` 后才析构 trivial_task 临时对象（C++ 临时对象销毁顺序保证这一点）。

#### P0-2: `task_promise<void>` 析构函数读取非活跃 union 成员 (UB)

**文件**: `include/coronet/task.hpp` L320-324

**问题**:

```cpp
~task_promise() noexcept {
    if (is_detached_flag != is_detached && has_exception_) {
        //  ^^^^^^^^^^^^^^^^^^^^^^^^^^^^ 读取 is_detached_flag
        //  但当 has_exception_ 为 true 时，活跃成员是 exception_ptr！
        exception_ptr.~exception_ptr();
    }
}
```

当 `has_exception_ == true` 时，union 的活跃成员是 `exception_ptr`（通过 `construct_at` 构造）。此时读取 `is_detached_flag` 是读取非活跃成员，C++ 标准规定为未定义行为。

实际危害：在大多数编译器上"恰好工作"（因为 `exception_ptr` 的位模式不太可能等于 `-1ULL`），但严格来说是 UB，且未来编译器优化可能导致意外行为。

**修复方案**:

```cpp
~task_promise() noexcept {
    if (has_exception_) {
        exception_ptr.~exception_ptr();
    }
    // detached 任务：unhandled_exception 直接 rethrow 终止程序，has_exception_ 永远为 false
    // 非 detached 无异常：is_detached_flag 是 trivial 类型，无需析构
}
```

#### P1-1: `iocp_proactor::outstanding_work_` 在正常操作中从不递减

**文件**: `include/coronet/platform/iocp/iocp_proactor.hpp` L163, `src/coronet/detail/worker_meta.cpp`

**问题**: `work_started()` 在 `await_suspend` 中被调用（`++outstanding_work_`），但 `work_finished()` 从未在任何正常完成路径中被调用。代码注释声称 "在 handle_completion 中递减（work_finished）"，但 `handle_completion` 实际只做 `--requests_to_reap`（非原子、单线程计数器），完全未调用 `work_finished()`。

**后果**:
1. `outstanding_work_` 在运行期间单调递增，永不归零。
2. `deinit()` 的 drain 循环条件 `outstanding_work_ > 0` 永远为真。
3. drain 循环必须等待 `max_drain_rounds = 1000` 轮超时才退出（每轮 1ms = 总计约 1 秒）。
4. **每个 `io_context` 析构都额外耗时约 1 秒**，即使没有任何在途 I/O。

**修复方案**: 在 `worker_meta::handle_completion` 中（IOCP 平台）调用 `work_finished()`：

```cpp
void worker_meta::handle_completion(const platform::completion_info* info) noexcept {
    assert(requests_to_reap > 0);
    --requests_to_reap;
#if defined(CORONET_PLATFORM_WINDOWS)
    if (info->opaque) {
        auto* raw = static_cast<platform::iocp::iocp_operation*>(info->opaque);
        auto* p = static_cast<platform::iocp::iocp_proactor*>(proactor);
        p->work_finished();  // ← 新增：递减 outstanding_work_
        platform::iocp::recycle_operation(
            std::unique_ptr<platform::iocp::iocp_operation>{raw});
    }
#endif
}
```

#### P1-2: `counting_semaphore` 丢失唤醒竞态

**文件**: `include/coronet/co/semaphore.hpp` L57-59, `src/coronet/co/semaphore.cpp` L39-49

**问题**: `acquire_awaiter::await_ready()` 先 `fetch_sub(1)` 递减计数器，然后 `await_suspend()` 才将 awaiter 插入等待链表。在这两步之间存在竞态窗口：

```
线程A (acquire)                    线程B (release)
─────────────────                 ─────────────────
await_ready:
  fetch_sub(1) → counter: 0→-1
  return false (将挂起)
                                   release:
                                     fetch_add(1) → counter: -1→0
                                     old_counter=-1 < 0 → 慢路径
                                     try_release() → awaiting_ 为空
                                     返回 nullptr, 无唤醒
await_suspend:
  插��� awaiting_ 链表
  挂起...
```

此后 `counter_ == 0`。下次 `release()` 看到 `old_counter == 0 >= 0`，直接返回不唤醒。**等待者永久挂起**。

**修复方案（经典模式）**: 在 `await_suspend` 插入链表后，重新检查 counter。如果 counter 已被 release 补回，从链表移除自己并重试。

或者改用不在 await_ready 中递减的方案 — 仅检查 `counter > 0` 并 CAS 递减，失败则挂起。但这需要更复杂的 awaiter 协议。

#### P1-3: `win_accept::issue_io` 错误路径 `WSAGetLastError` 被 `closesocket` 覆盖

**文件**: `include/coronet/platform/iocp/iocp_win_io.hpp` L356-375

**问题**:

```cpp
if (!ok) {
    DWORD err = ::WSAGetLastError();  // ← 读取 AcceptEx 的错误码
    if (err == WSA_IO_PENDING) { ... }
    else {
        ::closesocket(accept_socket_);  // ← closesocket 可能覆盖 WSA 错误码！
        finish_issue(1, 0);  // ← finish_issue 内部再次调用 WSAGetLastError()
        //   → 得到的是 closesocket 的错误码，不是 AcceptEx 的！
    }
}
```

**修复方案**: `finish_issue` 不应自己调用 `WSAGetLastError()`。应由调用方传入错误码：

```cpp
void finish_issue(DWORD ioresult, DWORD bytes, DWORD errcode = 0) noexcept {
    // ...
    io_info_.result = -static_cast<int32_t>(errcode);
    // ...
}
```

#### P2-1: `channel<T,0>` 会合模式对 T 的要求超出 `move_constructible` 约束

**文件**: `include/coronet/co/channel.hpp` L49, L200-249

所有 channel 特化都用 `template<std::move_constructible T, size_t capacity>` 约束，但 `channel<T,0>` 的 `stored_` 声明为 `T stored_{}`（要求默认构造）且用 `stored_ = T(...)` 赋值（要求可赋值）。而 N-slot 和 single-slot 用 `uninitialized_buffer<T>` / `std::optional<T>`，仅需 `move_constructible`。

**修复方案**: 将 `channel<T,0>` 的 `stored_` 改为 `uninitialized_buffer<T>`。

#### P2-2: `all_void_state` 缺少缓存行对齐

**文件**: `include/coronet/co/when_all.hpp` L118-122

`when_all_state<ResultTuple>` 有 `alignas(config::cache_line_size)` 防止 `remaining` 原子与 shared_ptr 控制块的引用计数共享缓存行。但 `all_void_state` 没有对齐。在高并发 `all(void_tasks...)` 场景下，多个子任务在不同 io_context 上 `fetch_sub(remaining)` 时，会与 `shared_ptr` 引用计数操作产生 false sharing。

**修复方案**: 添加 `alignas(config::cache_line_size)`。

#### P2-3: `co_spawn` 对未启动的 io_context 从第三线程调用存在数据竞争

**文件**: `src/coronet/detail/worker_meta.cpp` L66-76

当 `ready_count == 0`（所有 io_context 都未 start）时，从线程 C 调用 `io_context_B.co_spawn(task)` 会走到 `forward_task`，执行非原子的 SPSC push。线程 C 的写入与 io_context_B 的事件循环线程（后续通过 `start()` 创建）之间没有 happens-before 关系。

**修复方案**: 当 `this_thread.worker != this` 时，无论 `ready_count` 如何，都走 `co_spawn_cross`（mutex 保护的线程安全路径）：

```cpp
void worker_meta::co_spawn_auto(std::coroutine_handle<> handle) noexcept {
    if (detail::this_thread.worker != this) {
        co_spawn_cross(handle);
        return;
    }
    forward_task(handle);
}
```

#### P3-1: 组合子直接 `awaiting.resume()` 导致调用栈增长

**文件**: `include/coronet/co/when_all.hpp` L81-83, L131-133

`when_all_state::count_down()` 和 `all_run_void` 直接调用 `awaiting.resume()`，在子任务线程上同步恢复调用者协程。如果调用者协程恢复后又触发更多协程恢复，调用栈会累积。`mutex` 和 `condition_variable` 正确地使用 `co_spawn()` 将恢复投递到 SPSC 环，避免了此问题。

**建议**: 可接受现状（设计权衡），但在文档中标注"组合子在多级嵌套时可能栈增长"。

#### P3-2: `io_context::started_` 非原子布尔

**文件**: `include/coronet/io_context.hpp` L107

`started_` 在 `start()` 中写入（可能从任意线程调用），在析构函数中读取。非原子 bool 的并发读写是 UB。实际危害低（start 通常在构造后立即调用，析构在 join 后），但应改为 `std::atomic<bool>` 或加文档约束。

#### P3-3: `forward_task` SPSC 溢出回退可能导致活锁

**文件**: `src/coronet/detail/worker_meta.cpp` L138-156

SPSC 环满时回退到 `cross_queue`。下次 `drain_cross_thread` 将 `cross_queue` 内容 swap 出来再 push 到 SPSC。如果 SPSC 仍满，又回退到 `cross_queue`，形成活锁。实际危害低（需要 16384+ 个同时就绪协程），但可在 drain 后检查 SPSC 剩余容量，不足时跳过本轮 drain。

#### P3-4: `async_io.hpp` 中 `timeout(auto dur)` 使用缩写函数模板

**文件**: `include/coronet/async_io.hpp` L122

`auto dur` 参数使每个不同的 `chrono::duration` 类型都生成一份实例化。建议用概念约束：

```cpp
template<typename Rep, typename Period>
auto timeout(std::chrono::duration<Rep, Period> dur) noexcept
```

### API 设计审查

#### 正面

| API | 评价 |
|-----|------|
| `task<T>` | `[[nodiscard]]` + 唯一所有权 + `when_ready()` 分离等待与取值 — 设计优秀 |
| `tcp_acceptor` 三种 accept | `accept()`（兼容）/ `accept_socket()`（RAII）/ `accept_with_peer()`（含对端地址）— 分层清晰 |
| `async::recv/send/...` | 统一工厂 + `[[nodiscard]]` + `std::span` 参数 — 跨平台一致 |
| `transport` concept | C++20 concept 约束，为 TLS/QUIC 扩展预留 — 设计前瞻 |
| `mutex::lock_guard()` | 返回 RAII guard，避免忘记 unlock — 安全 |

#### 待改进

| 问题 | 建议 |
|------|------|
| `async::timeout(auto dur)` 缩写模板 | 用显式 `template<Rep, Period>` 约束 |
| `channel<T,0>` 对 T 的隐式要求 | 改用 `uninitialized_buffer<T>` 与其他特化一致 |
| `inet_address::resolve()` 是同步阻塞 | 文档已标注，但缺少异步版本（`resolve_async`） |
| `task<void>::detach()` 后无法获知完成 | 可考虑 `detach()` 接受回调或返回 `shared_task` |

### 修复优先级建议（2026-07-26）

| 优先级 | 问题 | 修复复杂度 | 影响范围 |
|--------|------|-----------|---------|
| **P0-1** | trivial_task 帧泄漏 | 低（加析构函数） | 所有 `cv.wait(pred)` 调用 |
| **P0-2** | task_promise<void> 析构 UB | 极低（改条件判断） | 所有 `task<void>` 异常路径 |
| **P1-1** | outstanding_work_ 不递减 | 低（加一行调用） | Windows io_context 析构 |
| **P1-2** | semaphore 丢失唤醒 | 中（改 awaiter 协议） | 多线程 semaphore |
| **P1-3** | win_accept 错误码覆盖 | 低（传参替代 WSAGetLastError） | IOCP accept 错误路径 |
| **P2-1** | channel<T,0> 类型要求 | 中（改用 uninitialized_buffer） | 会合模式 channel |
| **P2-2** | all_void_state 缓存行 | 极低（加 alignas） | 高并发 all(void...) |
| **P2-3** | co_spawn 第三线程竞态 | 低（去掉 ready_count 检查） | 跨线程 co_spawn |

### 总结

Coronet 的架构设计水平很高 — 静态多态、缓存行意识、CRTP 零开销抽象、IOCP 操作回收等设计均达到了工业级协程库的标准。代码注释详尽，约定明确。

本次审查发现的问题集中在：
1. **协程帧生命周期管理**（P0-1 trivial_task 泄漏）— 最紧急
2. **UB 修复**（P0-2 union 活跃成员）— 简单修复
3. **计数器/错误码正确性**（P1-1/P1-3）— Windows 平台特有
4. **并发原语竞态**（P1-2 semaphore）— 多线程场景

建议按优先级逐个修复，每个修复后运行 `ctest` 验证不引入回归。

---

## 2026-07-25 ~ 2026-07-26: 核心模块审查

### task 模块深度审查

**审查范围**: `include/coronet/task.hpp`、`include/coronet/detail/task_info.hpp`、`include/coronet/detail/tasklike.hpp`、`include/coronet/detail/trivial_task.hpp`、`test/task_gtest.cpp`、`test/ft_task.cpp`

#### P0-1: `unhandled_exception()` 对未初始化 union 成员赋值 — **崩溃 (0xc0000005)**

**文件**: `task.hpp` — `task_promise<T>::unhandled_exception()` 和 `task_promise<void>::unhandled_exception()`

**问题**: `task_promise<T>` 使用 union 存储 `value` 和 `exception_ptr`（互斥）。构造时 union 成员未初始化。当 `unhandled_exception()` 执行 `exception_ptr = std::current_exception()` 时：

1. `std::exception_ptr::operator=` 首先尝试释放旧值（调用 `_Decref`）
2. 旧值是垃圾内存 → 解引用垃圾指针 → **access violation (0xc0000005)**
3. GCC/Clang 的 `exception_ptr` 实现恰好先检查 null，所以没暴露
4. MSVC 的实现不检查 null，直接释放 → 崩溃

此 bug 此前被 `ft_task.cpp` 掩盖 — 该测试在协程内部 try-catch 捕获异常，异常不会传播到 `result()` 调用处。新的 `task_gtest.cpp` 直接通过 `result()` 获取异常时暴露了此问题。

**修复**: 使用 `std::construct_at` 替代直接赋值（与 `return_value` 已有模式一致）：

```cpp
// task_promise<T>
void unhandled_exception() noexcept {
    std::construct_at(&exception_ptr, std::current_exception());
    state = value_state::exception;
}

// task_promise<void>
void unhandled_exception() {
    if (is_detached_flag == is_detached) {
        std::rethrow_exception(std::current_exception());
    } else {
        has_exception_ = true;
        std::construct_at(&exception_ptr, std::current_exception());
    }
}
```

#### P0-2: `task_promise<void>::result()` 访问非活跃 union 成员 — UB

**文件**: `task.hpp` 第 422-425 行

**问题**: `task_promise<void>` 使用 union 存储两种互斥状态：

```cpp
union {
    uintptr_t is_detached_flag;       // 构造时初始化为 0
    std::exception_ptr exception_ptr;  // 仅在 unhandled_exception 时构造
};
bool has_exception_{false};
```

当 task 未分离且无异常时，union 的活跃成员是 `is_detached_flag`（值 0）。但 `result()` 直接检查 `if (this->exception_ptr)` —— 这是在访问**非活跃 union 成员**，技术上是 UB。

**修复**:

```cpp
void result() const {
    if (has_exception_) [[unlikely]] {
        std::rethrow_exception(this->exception_ptr);
    }
}
```

#### P0-3: `await_resume` 空句柄解引用 — release 模式 UB

**文件**: `task.hpp` — `awaiter_base::await_ready()` / `await_suspend()` / `await_resume()`

**问题**: 当 task 被 move-from 后（`handle = nullptr`），如果用户仍尝试 `co_await` 它：

1. `await_ready()` 检查 `!handle_ || handle_.done()` → null handle 返回 **true**
2. 跳过 `await_suspend`，直接调用 `await_resume()`
3. `await_resume()` 中 `assert(this->handle_ && "broken_promise")` 在 release 模式下被移除
4. `this->handle_.promise()` 解引用 null 指针 — **UB/崩溃**

**修复**: `await_ready()` 对 null handle 返回 false（走 `await_suspend`），`await_suspend` 中 `std::terminate()`：

```cpp
constexpr bool await_ready() const {
    return handle_ && handle_.done();  // null → false → 走 await_suspend
}
constexpr auto await_suspend(std::coroutine_handle<> awaiting) noexcept {
    if (!handle_) [[unlikely]] std::terminate();
    handle_.promise().set_parent(awaiting);
    return handle_;
}
```

#### P0-4: `detach()` 对非 void `task<T>` 静默泄漏协程帧

**文件**: `task.hpp` — `task::detach()`

**问题**: 对于 `task<int>` 等非 void task：
1. `detach()` 不设置 `is_detached_flag`
2. `handle = nullptr` 后析构函数不会销毁帧
3. 如果协程已执行到 `final_suspend`，帧永远不会被 `destroy()` — **内存泄漏**

**修复**: 使用 C++20 `requires` 约束，使 `detach()` 仅对 `task<void>` 存在：

```cpp
void detach() noexcept requires std::is_void_v<T> {
    handle.promise().is_detached_flag = promise_type::is_detached;
    handle = nullptr;
}
```

同步更新 `ft_task.cpp`，移除对 `task<int>::detach()` 的调用（编译期已禁止）。

#### P1-1: `is_ready()` 缺少 `noexcept`

`is_ready()` 只调用 `handle.done()`（noexcept）和指针判空，自身不可能抛异常，但未声明 `noexcept`。**修复**: 添加 `noexcept`。

#### P1-2: `result()` 在 `state==mono` 时 release 模式返回未初始化值

**文件**: `task.hpp` — `task_promise<T>::result()` 两个重载

```cpp
if (state == value_state::exception) [[unlikely]] {
    std::rethrow_exception(exception_ptr);
}
assert(state == value_state::value);  // release 模式下消失
return value;  // state==mono 时返回未初始化的 value — UB
```

**修复**: 在 assert 后加 terminate 守卫：

```cpp
T &result() & {
    if (state == value_state::exception) [[unlikely]] {
        std::rethrow_exception(exception_ptr);
    }
    assert(state == value_state::value);
    if (state != value_state::value) [[unlikely]] {
        std::terminate();
    }
    return value;
}
```

两个重载（`&` 和 `&&`）都加守卫。

#### P1-3: 测试覆盖严重不足 — 已修复

**文件**: `test/task_gtest.cpp`

原有 4 个测试（CreateValue, CreateVoid, MoveSemantics, Detach），且没有任何测试实际 `co_await` 一个 task 并验证返回值、没有异常传播测试、`run_task` 辅助函数是死代码。

**修复**: 全面重写测试文件，删除死代码 `run_task`，新增 21 个测试：

| 测试名 | 覆盖点 |
|--------|--------|
| `CreateValue` / `CreateVoid` | 基本创建 |
| `MoveSemantics` | move 构造/赋值 |
| `DetachVoid` | task<void>::detach() + 手动 resume |
| `AwaitValue` / `AwaitVoid` | 实际 co_await 验证返回值/完成 |
| `ExceptionPropagation` | 异常通过 co_await 传播到 result() |
| `ExceptionInt` / `ExceptionVoid` | task<int>/task<void> 异常传播 |
| `NestedException` | 嵌套协程异常传播 |
| `WhenReady` | when_ready() 不抛异常 |
| `MoveFromAwait` | co_await moved-from task 的安全终止 |
| `TaskRef` / `TaskRefException` | task<T&> 的引用返回和异常传播 |
| `Swap` | task swap 语义 |
| `IsReady` | is_ready() 在不同状态下的行为 |
| `GetHandle` | get_handle() 返回有效句柄 |
| `CoroutineReturnValue` | co_return 值语义 |
| `LargeReturn` | 大对象返回（触发堆分配） |
| `ChainAwait` | 链式 co_await |
| `DetachedNoCrash` | detached task 不崩溃 |

#### P2 — 代码质量 / 一致性

| 项 | 描述 |
|----|------|
| P2-1 | `detach()` 注释过时 — 更新两处注释（`detach()` 文档注释和 `task_final_awaiter` 流程图注释） |
| P2-2 | `task<T&>::result()` 缺少 `const` — 添加 `const` 保持一致 |
| P2-3 | `task_info` 的 `type_tag` 未被使用 — 添加 `[[maybe_unused]]` 标注"预留扩展" |

#### 关键发现：union 成员安全构造

`task_promise<T>` 的 union 中 `value` 和 `exception_ptr` 共享存储。这是性能优化（避免动态分配），但带来了安全陷阱：

```
union {
    T value;                    // return_value 时构造
    std::exception_ptr exception_ptr;  // unhandled_exception 时构造
};
value_state state = value_state::mono;  // 初始状态：两者都未构造
```

**陷阱**: 对未构造的 union 成员��用 `operator=`（而非 placement-new / `construct_at`）会触发该成员的赋值运算符，而赋值运算符可能尝试释放"旧值"（实际上是垃圾内存）。

**规则**: union 成员的构造和赋值必须使用 `std::construct_at`（C++20）或 placement-new。直接赋值是 UB。

**已修复的位置**:
- `task_promise<T>::unhandled_exception()` — `exception_ptr = ...` → `std::construct_at(&exception_ptr, ...)`
- `task_promise<void>::unhandled_exception()` — 同上

**已有的正确模式**（无需修改）:
- `task_promise<T>::return_value()` — 已使用 `std::construct_at`
- `shared_task` 的 storage — 已使用 `std::construct_at`（第一阶段 Batch 3 修复）

---

### io_context 调度中心审查

**审查范围**: `io_context.hpp/cpp`、`worker_meta.hpp/cpp`、`thread_meta.hpp`、`io_context_meta.hpp/cpp`、`spsc_cursor.hpp`、`task_info.hpp`、`iocp_proactor.hpp/cpp`、`task.hpp`（调度相关部分）、`trivial_task.hpp`、`condition_variable.hpp/cpp`、`mutex.hpp/cpp`

| 严重度 | 数量 | 说明 |
|:---:|:---:|------|
| **P0** | 3 | 数据竞争 / UB / 屏障死锁 |
| **P1** | 5 | 健壮性 / 关闭安全 / 资源泄漏 |
| **P2** | 4 | 性能 / 代码质量 / 可维护性 |

#### P0-1: `drain_cross_thread()` 无锁读取 `cross_queue.empty()` — 数据竞争

**文件**: `src/coronet/detail/worker_meta.cpp:100-101`

**问题**: `cross_queue.empty()` 读取 `std::vector` 的 `size()` 字段，但未持有 `cross_mtx`。另一线��可能并发执行 `co_spawn_cross()` 中的 `cross_queue.push_back(handle)`（在锁内）。`std::vector` 不是线程安全的——并发读取 `size()` 和 `push_back` 是**未定义行为**（UB）。

**修复**: 将 empty 检查移入锁内：

```cpp
void worker_meta::drain_cross_thread() noexcept {
    thread_local std::vector<std::coroutine_handle<>> batch;
    {
        std::lock_guard lock(cross_mtx);
        if (cross_queue.empty()) return;
        batch.swap(cross_queue);
    }
    for (auto h : batch) {
        forward_task(h);
    }
    batch.clear();
}
```

**性能影响**: 无竞争时 `std::mutex` lock+unlock 仅需一次 CAS（~10ns），可忽略。

#### P0-2: `this_io_context()` 空指针解引用 — UB

**文件**: `src/coronet/io_context.cpp:189-191`

```cpp
io_context& this_io_context() noexcept {
    return *detail::this_thread.ctx;   // ← ctx 可能为 nullptr
}
```

**问题**: 从非 io_context 线程（如 `main()` 线程、`std::thread` 裸线程）调用时，`this_thread.ctx` 为 `nullptr`。解引用空指针是 UB。

**修复方案**: 返回指针或添加断言：

```cpp
io_context* this_io_context() noexcept {
    return detail::this_thread.ctx;
}
```

#### P0-3: 全局启动屏障 `create_count` 不随析构递减 — 阶段性创建死锁

**文件**: `src/coronet/io_context.cpp:49-55`、`src/coronet/detail/io_context_meta.cpp:22-35`

**问题**: `create_count` 只增不减。场景：

1. 创建 io_context A、B（`create_count=2`）
2. 销毁 A、B（`create_count` 仍为 2）
3. 创建 C（`create_count=3`）
4. 启动 C（`ready_count=1`）
5. C 的 `wait_all_ready` 检查 `1 >= 3` → **永久阻塞**

**修复方案**: 在析构函数中递减 `create_count`：

```cpp
io_context::~io_context() noexcept {
    can_stop();
    join();
    auto old = detail::g_io_context_meta.create_count.fetch_sub(1, std::memory_order_release);
    if (old - 1 <= detail::g_io_context_meta.ready_count.load(std::memory_order_acquire)) {
        detail::g_io_context_meta.ready_count.notify_all();
    }
}
```

#### P1-1: `will_stop_` 加载使用 `memory_order_relaxed` — C++ 内存模型技术性数据竞争

**文件**: `src/coronet/io_context.cpp:128`

`can_stop()` 的 store（seq_cst）和循环的 load（relaxed）之间没有 happens-before 关系。在 x86/Windows 上因 TSO（Total Store Order）+ 内核同步实际不会出问题，但在 ARM（弱内存模型）上可能观测到 store 后 store 仍未可见。

**修复方案**: load 改为 `acquire`。x86 上零开销（编译为普通 `mov`），ARM 上加一条 `dmb ishld`，可忽略。

#### P1-2: `requests_to_reap` 无下溢检测 — 会计错误可能导致负值

**文件**: `src/coronet/detail/worker_meta.cpp:188`

如果因 bug（如重复处理完成事件）导致 `handle_completion` 在 `requests_to_reap == 0` 时被调用，计数器变为 -1。后续所有 I/O 会计全部偏移。

**修复方案**: 添加断言 `assert(requests_to_reap > 0)`。

#### P1-3: `iocp_operation` 空闲链表复用 `OVERLAPPED::Internal` 字段 — 调试困难

**文件**: `src/coronet/platform/iocp/iocp_proactor.cpp:62,78`

空闲链表通过 `OVERLAPPED::Internal` 字段串联节点。`Internal` 在正常 I/O 中用于存储错误码（NTSTATUS）。如果因 double-free 或 use-after-free 导致回收态的 `Internal` 被覆盖，链表会静默损坏，极难调试。

**修复方案**: 添加独立的 `next_` 成员（每个 `iocp_operation` 多 8 字节，在 128 个回收缓存中总计 1KB，可忽略）。

#### P1-4: `co_spawn` 自由函数在非 io_context 线程静默丢弃任务

**文件**: `src/coronet/io_context.cpp:181-186`

从非 io_context 线程调用时，`entrance` 的析构函数调用 `handle.destroy()`，协程被销毁且永远不执行。用户得不到任何提示。

**修复方案**: 添加日志 `log::w("[co_spawn] called from non-io_context thread, task dropped\n")`。

#### P1-5: 事件循环退出时不清理残留协程 — 帧泄漏

**文件**: `src/coronet/io_context.cpp:128-148`

`can_stop()` 设置停止标志后，事件循环在当前迭代结束后退出。SPSC 环和跨线程队列中残留的协程句柄永远不会被 `resume()`。对于 detached task<void>，协程帧永远不会被销毁 → **内存泄漏**���对于非 detached task，父协程的 `co_await` 永远不返回 → **死锁**。

**修复方案**: 在 `deinit()` 前添加 drain 阶段：

```cpp
void io_context::run() {
    // ... 事件循环 ...

    // Drain phase: 恢复所有残留协程
    drain_cross_thread();
    do_worker_part();
    for (int i = 0; i < 3 && worker_.has_task_ready(); ++i) {
        drain_cross_thread();
        do_worker_part();
    }

    deinit();
    detail::this_thread = {};
}
```

#### P2: 性能 / 代码质量

| 项 | 描述 | 修复复杂度 |
|----|------|:---:|
| **P2-1** 事件循环 `% 1000` 整数除法 | 改用 `& 1023` 或 `if constexpr` 消除日志 | 低 |
| **P2-2** `thread_local batch` 不收缩 | `clear()` 后检查 capacity，超过阈值 `shrink_to_fit()` | 低 |
| **P2-3** `iocp_proactor::deinit()` 三轮排空脆弱 | 改用原子计数器跟踪后台线程数量 | 高 |
| **P2-4** `forward_task` public + SPSC unsafe | 改为 `private` 或加断言 | 低 |

#### 架构评价

**优秀的设计**:

| 设计 | 评价 |
|------|------|
| SPSC 环无锁调度（同线程） | 零锁、零等待，生产者/消费者均为事件循环线程 |
| 跨线程 co_spawn 的 mutex 队列 + 唤醒优化 | 仅在队列从空变非空时唤醒，最小化系统调用 |
| 缓存行隔离 | hot/cold 分离，避免 false sharing |
| 静态多态 Proactor（编译期选择） | 零虚表开销，值语义 |
| iocp_operation 对象池回收 | 避免 high-frequency I/O 的堆分配 |
| user_data 指针编码（8 字节对齐 + 3 位 tag） | 零查表，O(1) 解码 |
| 全局启动屏障 | 消除跨 io_context co_spawn 的竞态窗口 |

---

### net/ 模块审查

**审查范围**: `include/coronet/net/{socket.hpp, acceptor.hpp, inet_address.hpp}` + `src/coronet/net/{socket.cpp, inet_address.cpp}`

| 严重度 | 数量 | 说明 | 修复状态 |
|:---:|:---:|------|:---:|
| **P0** | 4 | 资源泄漏 / UB / 功能错误 | ✅ 全部修复 |
| **P1** | 10 | 健壮性 / 功能缺失 / 异步一致性 | ✅ 全部修复 |
| **P2** | 6 | 代码质量 / 完备性 / 一致性 | ✅ 全部修复 |
| **架构** | 3 | TLS / QUIC 可扩展性设计 | ✅ Phase 3 已完成 / 📋 Phase 4 待执行 |

#### P0-1: `socket::operator=(socket&&)` 泄漏当前套接字

**文件**: `include/coronet/net/socket.hpp:82-88`

移动赋值直接覆盖 `sockfd_`，原套接字永久泄漏。这是 RAII 类的经典 bug。

**修复**: 先关闭当前套接字再赋值，或复用 swap 惯用法：

```cpp
socket& operator=(socket&& other) noexcept {
    socket tmp(std::move(other));
    swap(tmp);
    return *this;
}
```

#### P0-2: `inet_address(string_view, port)` 传递非 null 终止字符串给 `inet_pton`

**文件**: `src/coronet/net/inet_address.cpp:14-33`

`std::string_view::data()` **不保证** null 终止。`inet_pton` 要求 C 字符串（null 终止）。如果 `string_view` 是某个字符串的子串，`inet_pton` 会越界读取 → UB。

**修复**: 构造临时 `std::string` 确保 null 终止。

#### P0-3: `inet_address::resolve_all` 传递非 null 终止字符串给 `getaddrinfo`

**文件**: `src/coronet/net/inet_address.cpp:124-142`

与 P0-2 相同问题。`hostname.data()` 不保证 null 终止。

**修复**: `std::string host_str{hostname};` → `::getaddrinfo(host_str.c_str(), ...)`.

#### P0-4: IOCP `win_accept::create_accept_socket()` 硬编码 `AF_INET`

**文件**: `include/coronet/platform/iocp/iocp_win_io.hpp:310`

`AcceptEx` 要求 accept socket 的地址族与 listen socket 一致。如果 listen socket 是 IPv6（`AF_INET6`），accept socket 创建为 `AF_INET` 会导致 `AcceptEx` 失败。

**修复**: 从 listen socket 获取地址族，传入 `create_accept_socket(int family)`。

#### P1: 健壮性 / 功能缺失 / 异步一致性

| 项 | 描述 | 修复 |
|----|------|------|
| **P1-1** | `acceptor::accept()` 返回裸 `int` fd — RAII 缺口 | 添加 `accept_socket()` 返回 `socket` |
| **P1-2** | `bind()`/`listen()` 失败时 `std::abort()` — 库不应终止进程 | 改为 `std::system_error` 异常 |
| **P1-3** | `create_tcp`/`create_udp` 用 `assert` 检查 fd — Release 模式静默失败 | 改为异常 |
| **P1-4** | `set_reuse_addr`/`set_tcp_no_delay` 静默忽略错误 | 统一错误处理策略（异常） |
| **P1-5** | DNS 解析是同步阻塞调用 — 与异步库理念冲突 | 短期文档标注，长期实现 `resolve_async()` |
| **P1-6** | `acceptor::accept()` 不返回对端地址 | 添加 `accept_with_peer()` 返回 `accept_result{conn, peer}` |
| **P1-7** | `set_nonblocking()` 忽略 `fcntl` 错误 | 检查返回值 |
| **P1-8** | `local_addr()`/`peer_addr()` 静默返回 AF_UNSPEC | 返回 `std::optional<inet_address>` |
| **P1-9** | 无 `SO_REUSEPORT` 支持 | 添加 `set_reuse_port(bool)` |
| **P1-10** | `to_ip_port()` 不对 IPv6 加方括号 | IPv6 加 `[ ]`，遵守 RFC 3986 |

#### P2: 代码质量 / 完备性

| 项 | 描述 |
|----|------|
| **P2-1** | 无 UDP `recvfrom` / `sendto` — 需要平台层支持 |
| **P2-2** | 只有 `shutdown_write()`，缺少 `shutdown_read()` / `shutdown_both()` |
| **P2-3** | `close()` 未保护已关闭的套接字 — 加 guard |
| **P2-4** | 缺少常用套接字选项（keepalive、buffer size、linger） |
| **P2-5** | `acceptor` backlog 硬编码 `SOMAXCONN` — 构造函数增加可选参数 |
| **P2-6** | `bind()` 中 `ntohs(addr.port())` 多余 — `addr.port()` 已返回 host byte order |

#### 架构设计 — TLS / QUIC 可扩展性

**当前问题**: `socket` 类直接绑定 TCP 语义，没有传输层抽象。无法插入 TLS 层或支持 QUIC。

**推荐架构**: 传输层抽象（概念多态），用 C++20 concepts 定义传输接口：

```
            用户代码 (task<>)
         co_await conn.recv(buf);
         co_await conn.send(data);
                   │ concept transport
        ┌──────────┼──────────┐
        ▼          ▼          ▼
   tcp_socket  tls_socket  quic_stream
```

**重构路径（渐进式）**:

```
Phase 1 (当前): 修复 P0/P1 bug，不改架构 ✅
Phase 2: 传输层抽象 — transport / listener concepts
Phase 3: TLS 支持 — tls_context / tls_socket / tls_acceptor ✅ 已完成
Phase 4: QUIC 支持 — quic_engine / quic_connection / quic_stream 📋 待执行
```

#### API 设计评价

**合理的设计**: socket 移动语义/不可拷贝、`explicit socket(int fd)` 防止隐式转换、`[[nodiscard]]` 异步操作、`acceptor` 构造即 bind+listen、`inet_address` 用 `sockaddr_storage` 统一存储。

**命名建议**:

| 当前 | 建议 | 理由 |
|------|------|------|
| `socket` | `tcp_socket` | 明确传输层类型 |
| `acceptor` | `tcp_acceptor` | 同上 |

#### 性能审查

**优势**: Linux `accept4` 一次设置 `SOCK_NONBLOCK|SOCK_CLOEXEC`、IOCP `AcceptEx` + 预创建 socket、静态多态零虚表开销、io_uring 链式 `co_await`。

**改进建议**: accept 避免协程帧分配、SO_REUSEPORT 替代 round-robin、MSG_ZEROCOPY 大块传输减少拷贝、TCP_FASTOPEN 减少 RTT、`recvmmsg`/`sendmmsg` 批量处理。

---

### 第一阶段：全量 Code Review（缓存行 / C++20 现代化 / 跨平台）

**审查维度**: C++20 现代化、裸指针减少、CPU 缓存利用、架构设计、性能优先

**审查范围**: 41 个头文件 (`include/coronet/`)、12 个源文件 (`src/coronet/`)、6 个测试文件 (`test/`)

#### Batch 1 — P0 性能关键

**1.1 `spsc_cursor.hpp` — SPSC 环形游标缓存行隔离**

**文件**: `include/coronet/detail/spsc_cursor.hpp`

**问题**: SPSC 环形游标的 `head_` 和 `tail_` 位于同一缓存行，生产者和消费者线程交替写入导致 false sharing。

**修复**: 引入 `padded_atomic` 结构体，每个原子变量独占一个缓存行。使用 `std::conditional_t` 在编译期选择原子/非原子版本（`is_safe` 标志），`[[no_unique_address]]` 在非安全模式下零开销。

```cpp
struct alignas(config::cache_line_size) padded_atomic {
    std::atomic<cur_t> v{0};
};
[[no_unique_address]]
std::conditional_t<is_safe, padded_atomic, cur_t> head_{};
[[no_unique_address]]
std::conditional_t<is_safe, padded_atomic, cur_t> tail_{};
```

**1.2 `worker_meta.hpp` — 热冷数据分离**

**文件**: `include/coronet/detail/worker_meta.hpp`

**问题**: 热数据（proactor 指针、请求计数器、reap 交换区）和冷数据（cross_mtx 互斥锁、cross_queue 跨线程队列）混在同一缓存行。

**修复**: 重排成员布局，热数据在前，冷数据在后，中间用 `alignas(cache_line_size)` 隔开：

```cpp
struct worker_meta {
    alignas(config::cache_line_size)
    platform::proactor_type* proactor{nullptr};

    int32_t  requests_to_reap   = 0;
    uint32_t requests_to_submit = 0;
    config::ctx_id_t ctx_id{0};
    std::vector<std::coroutine_handle<>> reap_swap{config::swap_capacity};
    spsc_cursor<config::cur_t, config::swap_capacity> reap_cur;

    alignas(config::cache_line_size)  // ← 缓存行边界
    std::mutex cross_mtx;
    std::vector<std::coroutine_handle<>> cross_queue;
};
```

**1.3 `iocp_win_io.hpp` — IOCP 线程池替代 per-op `std::thread().detach()`**

**文件**: `include/coronet/platform/iocp/iocp_win_io.hpp`

**问题**: `win_timeout`、`win_read`、`win_write` 中每次 I/O 操作都 `std::thread(lambda).detach()` 创建新线程。高并发下线程创建/销毁开销巨大，且 detach 线程在进程退出时可能访问已释放资源。

**修复**: 引入 `win_file_io_pool` 泄漏式单例线程池，所有文件 I/O 操作提交到共享线程池。泄漏式单例避免静态析构期的 use-after-free 和 join 挂起（OS 在进程退出时自动回收资源）。

**测试验证**: `async_io_iso_stress` 测试此前在 120 秒超时（实际逻辑 31 秒完成，但进程不退出），修复后 31.46 秒正常通过。

#### Batch 2 — P1 类型安全 + epoll 优化 + 缓存行

**2.1 `iocp_proactor.hpp` — `void*` → `HANDLE`**: 使用 Windows API 原生类型 `HANDLE` 替代 `void*`。

**2.2 `iocp_win_io.hpp` — C 风格转换 → `static_cast`**: 13 处 C 风格强制转换替换为 `static_cast`（`(SOCKET)sock_` → `static_cast<SOCKET>(sock_)` 6 处、`(uintptr_t)fd` → `static_cast<uintptr_t>(fd)` 6 处、`(__int64)off` → `static_cast<__int64>(off)` 1 处）。

**2.3 `epoll_lazy_io.hpp` — epoll 文件 I/O 优化**:

epoll 无法做内核原生异步文件 I/O（不像 io_uring 的 `IORING_OP_READ` 或 IOCP 的 `ReadFile` + completion key），线程池是必需品。三项优化：

1. **pipe2 → eventfd**: 单 fd、内核维护计数器、`read` 自动重置、`write` 累加
2. **消除 `new`/`delete` 堆分配**: 将上下文嵌入 awaiter 对象本身（协程帧上），零堆分配
3. **线程池泄漏式单例**: 与 IOCP 的 `win_file_io_pool` 设计对称

**2.4 缓存行隔离**: `spinlock.hpp` (`alignas(cache_line_size)`)、`when_all.hpp` (`alignas(cache_line_size)` `when_all_state`)、`io_context.hpp` (`will_stop_` 独占缓存行)。

#### Batch 3 — P2 C++20 现代化

**3.1 `shared_task.hpp` — placement-new → `std::construct_at`**: C++20 推荐的 placement-new 替代方案，类型更安全、可在常量表达式中使用。

**3.2 `lock_guard.hpp` — `unlockable` concept**: 使用 `static_assert(requires(...))` 而非 `template<unlockable Lockable>` 约束（概念约束会改变模板参数推导行为，导致 MSVC C2784 推导失败）。

**3.3 `io_context_meta.cpp` / `io_context.cpp` — `std::atomic::wait/notify_all`**: 使用 C++20 `std::atomic::wait()` + `notify_all()` 替代 spin-yield 轮询。`atomic::wait` 在 Linux 上使用 `futex`，在 Windows 上使用 `WaitOnAddress`，内核级挂起，零 CPU 消耗。

**3.4 `worker_meta.cpp` — SPSC 溢出降级 fallback**: SPSC 环形缓冲区溢出时不再 `std::abort()`，而是降级到 cross_queue（互斥锁保护的备选队列）。

---

### 验证结果总览

**第一阶段**: MSVC Release 72 目标全部成功，ctest 26/26 全部通过（0 失败），总耗时 110.32 秒

**第二阶段**: MSVC Release 全部成功，ctest 26/26 全部通过（0 失败），总耗时 115.82 秒

| 测试 | 耗时 | 状态 |
|------|------|:---:|
| task_gtest | 0.22s | ✅ |
| generator_gtest | 0.09s | ✅ |
| channel_gtest | 0.11s | ✅ |
| shared_task_gtest | 0.11s | ✅ |
| ft_task ~ when_some (15项) | 0.11~2.09s | ✅ |
| channel | 8.21s | ✅ |
| cv_notify_all / cv_notify_one | 5.18s / 5.08s | ✅ |
| combinator_stress | 10.23s | ✅ |
| lazy_io_comprehensive | 15.32s | ✅ |
| **async_io_iso_stress** | **31.43s** | ✅ |
| stress_driver_ST / MT | 4.17s / 3.44s | ✅ |

**最终验证**:
- **编译**: MSVC Release 全部成功（exit code 0）
- **测试**: ctest 26/26 全部通过，0 失败，0 回归
- **关键压测**: async_io_iso_stress 31s ✅、combinator_stress 10s ✅、channel 8s ✅

---

## 关键设计决策回顾

### 1. epoll 线程池保留但优化

**用户提问**: 是否去掉 epoll 的线程池，与 io_uring/iocp 保持一致？

**结论**: 保留但优化。

**理由**: epoll 无法做内核原生异步文件 I/O。io_uring 有 `IORING_OP_READ`，IOCP 有 `ReadFile + completion key`，两者都在内核中完成 I/O 并通知用户空间。epoll 只能监听 fd 就绪事件，普通文件 fd 始终"就绪"（`epoll_wait` 对普通文件返回 `EPOLLERR`），所以必须用线程池 + `pread/pwrite` 模拟异步。

**优化方向**: 消除堆分配（ctx 嵌入 awaiter）、pipe2 → eventfd（单 fd、内核计数器）、线程池泄漏式单例。

### 2. IOCP 线程池对称设计

`win_file_io_pool`（IOCP）与 `file_io_pool`（epoll）设计对称，均为泄漏式单例。泄漏式单例避免静态析构期的 use-after-free 和 join 挂起。

### 3. lock_guard concept 用 static_assert

`template<unlockable Lockable>` 约束会改变模板推导行为导致 MSVC C2784 编译失败。改用 `static_assert(requires(...))` 在不改变推导行为的前提下提供等价的编译期检查。

### 4. generator.hpp 不修改

作为 P2502R2 (`std::generator`) 参考实现，保持原样。

### 5. union 成员构造统一用 `std::construct_at`

所有 union 成员的构造和赋值统一使用 `std::construct_at`（C++20），避免 `operator=` 对未初始化内存的危险操作。

### 6. 传输层抽象设计（概念多态）

保持项目"静态多态、零虚表开销"的设计哲学，用 C++20 concepts 定义 `transport` 接口，为 TLS 和 QUIC 提供扩展点。

---

## 附录：改动统计与文件清单

### 改动统计

| 阶段 | 批次 | 项数 | 文件数 |
|------|------|------|--------|
| 第一阶段 | Batch 1 (P0) | 3 | 3 |
| 第一阶段 | Batch 2 (P1) | 4 | 5 |
| 第一阶段 | Batch 3 (P2) | 4 | 4 |
| 第二阶段 (task) | P0 | 4 | 2 |
| 第二阶段 (task) | P1 | 3 | 2 |
| 第二阶段 (task) | P2 | 4 | 2 |
| 第三阶段 (net) | P0 | 4 | 4 |
| 第三阶段 (net) | P1 | 10 | 5 |
| 第三阶段 (net) | P2 | 6 | 3 |
| **合计（已修复）** | | **42 项** | **19 文件** |
| 2026-07-26 全面审查 | P0/P1/P2/P3 | 12 项 | 待修复 |
| 2026-07-27 TLS 审查 | P1/P2/P3 | 6 项 | 待修复 |

### 全部修改文件清单

| 文件 | 改动内容 |
|------|----------|
| `include/coronet/detail/spsc_cursor.hpp` | 缓存行隔离（padded_atomic） |
| `include/coronet/detail/worker_meta.hpp` | 热冷数据分离 |
| `include/coronet/detail/worker_meta.cpp` | SPSC 溢出降级 fallback |
| `include/coronet/platform/iocp/iocp_win_io.hpp` | win_file_io_pool + static_cast + IPv6 accept socket 地址族 |
| `include/coronet/platform/iocp/iocp_proactor.hpp` | void* → HANDLE |
| `include/coronet/platform/epoll/epoll_lazy_io.hpp` | eventfd + 零堆分配 + 泄漏单例 |
| `include/coronet/detail/spinlock.hpp` | alignas 缓存行 |
| `include/coronet/co/when_all.hpp` | when_all_state alignas 缓存行 |
| `include/coronet/io_context.hpp` | will_stop_ alignas 缓存行 |
| `include/coronet/shared_task.hpp` | std::construct_at |
| `include/coronet/detail/lock_guard.hpp` | unlockable concept (static_assert) |
| `src/coronet/detail/io_context_meta.cpp` | atomic::wait/notify |
| `src/coronet/io_context.cpp` | atomic::notify_all |
| `include/coronet/task.hpp` | union 安全构造、空句柄守卫、detach requires、noexcept、result terminate 守卫、const 一致性、注释更新、task_promise<void> 析构 UB |
| `include/coronet/detail/task_info.hpp` | type_tag [[maybe_unused]] |
| `test/task_gtest.cpp` | 全面重写，21 个测试 |
| `test/ft_task.cpp` | 移除 task<int>::detach() 调用 |
| `include/coronet/net/socket.hpp` | P0-1 operator= 泄漏、P1-3 assert→异常、P1-9 SO_REUSEPORT |
| `src/coronet/net/socket.cpp` | abort→异常、setsockopt 错误处理、fcntl 错误、optional 返回、ntohs 多余 |
| `src/coronet/net/inet_address.cpp` | string_view→string null 终止、IPv6 方括号、getaddrinfo null 终止 |
| `include/coronet/net/acceptor.hpp` | accept_socket()、accept_with_peer()、backlog 参数 |
| `include/coronet/net/tls/tls_context.hpp` | TLS 上下文（新增） |
| `include/coronet/net/tls/tls_socket.hpp` | TLS 套接字（新增） |
| `include/coronet/net/tls/tls_acceptor.hpp` | TLS 监听器（新增） |
| `cmake/Extra.cmake` | OpenSSL FetchContent fallback |
