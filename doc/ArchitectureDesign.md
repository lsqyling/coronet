# coronet 架构设计

> 版本: 2026-08-02 | 对应代码: build/3-ways 分支（P2-1 帧泄漏修复 + B1 ring 配置化 + B2 帧池后）
> 配套文档: [CodeReview](CodeReview.md)（审查与修复记录）| [TestReport](TestReport.md)（三端实测数据）

---

## 一、设计目标与定位

coronet 是一个 **C++20 原生协程、跨平台（Linux / Windows）、高性能异步 I/O 库**。

设计目标（按优先级）：

1. **性能**：吞吐与业界标杆（asio 协程版）同级或更优；单核效率（每 rps 的 CPU 成本）显著更优
2. **内存**：协程帧生命周期正确（不泄漏）、每操作零堆分配、总内存占用低
3. **跨平台**：一套代码，编译期自动选择 epoll / io_uring / IOCP 三后端，零运行期分派
4. **现代 C++**：C++20 协程 + Concepts + CRTP + 模板元编程，无虚表、无回调地狱

---

## 二、整体架构

```
┌─────────────────────────────────────────────────────────┐
│ 用户代码层                                              │
│   task<> / shared_task<> / generator<> / channel /      │
│   mutex / condition_variable / semaphore / when_all·any  │
├─────────────────────────────────────────────────────────┤
│ I/O 工厂层  async::recv / send / accept / connect /     │
│             read / write / timeout / yield               │
│             （函数体内零 #ifdef —— 全部转发给平台工厂）     │
├─────────────────────────────────────────────────────────┤
│ 平台适配层（编译期选择，CRTP 静态多态）                   │
│   ┌──────────┬──────────────┬──────────────┐            │
│   │  epoll   │   io_uring   │    IOCP      │            │
│   │ (默认)   │ (CORONET_    │ (Windows     │            │
│   │          │   IOURING=ON)│   自动)      │            │
│   └──────────┴──────────────┴──────────────┘            │
├─────────────────────────────────────────────────────────┤
│ 调度内核                                                  │
│   io_context 事件循环（四阶段，见 §三.2）                  │
│   worker_meta：SPSC 无锁环 + 跨线程队列 + I/O 计数器      │
├─────────────────────────────────────────────────────────┤
│ 内核 / 驱动                                               │
│   epoll / io_uring / IOCP + eventfd / PQCS 唤醒          │
└─────────────────────────────────────────────────────────┘
```

### 分层原则

| 层 | 职责 | 关键约束 |
|---|---|---|
| 用户层 | 业务协程，`co_await` 组合 | 不感知后端差异 |
| 工厂层 | `async_io.hpp` 统一入口 | **零 `#ifdef` 函数体**，编译期分派到 `detail::platform_io::make_*()` |
| 平台层 | awaiter 实现 + Proactor | 所有平台差异隔离在 `platform/`，上层无感知 |
| 调度层 | 事件循环 + 任务分发 | 与平台无关（IOCP/uring 差异被 Proactor 概念抹平） |

**核心设计决策 — 静态多态**：不用虚函数、不用运行时插件，`#ifdef` 在编译期锁定具体后端。
代价是"同一份源码需为每个后端各构建一次"（epoll/io_uring 两个构建目录），换来：

- 零虚表开销、零间接跳转
- awaiter 内联进协程帧，编译器可全优化
- 每个后端可为其机制特化（io_uring 的 SQE 链、IOCP 的对象池）

---

## 三、技术方案

### 3.1 协程模型：惰性 task + 对称转移调度

- `task<T>`：惰性、唯一所有权、父链内联恢复。`co_await` 子任务时，子协程完成
  后经 `final_suspend` **对称转移**（返回父协程句柄）直接恢复父链 —— 无回调、
  无中间调度器跳转
- `shared_task<T>`：引用计数多等待者（广播/扇出场景）
- `generator<T>`：P2502R2 标准参考实现
- `detach()`：fire-and-forget（服务器每连接会话的标准启动方式）

**对称转移 vs asio 的 awaitable_thread 帧栈泵**：coronet 无 wrapper 帧，
`co_spawn` 直接把协程句柄推入调度环；asio 每任务多一层 `entry_point` 帧 + 线程帧栈。
实测二者性能同级，coronet 内存更省（见 §六 对比表）。

### 3.2 事件循环：四阶段

`io_context::run()` 每轮循环：

```
drain_cross_thread()   ← 跨线程队列 → 并入 SPSC 环（eventfd/PQCS 唤醒）
do_worker_part()       ← SPSC 环弹出 → coroutine_handle::resume()
do_submission_part()   ← 提交 I/O（仅 io_uring：io_uring_enter）
do_completion_part()   ← 收割完成事件（CQE 解码 → handle_completion → forward_task）
```

- **同线程内部协程唤醒零锁**：SPSC 无锁环（`spsc_cursor`），单生产者单消费者
- **跨线程 co_spawn**：mutex 保护队列 + `eventfd`（Linux）/ `PostQueuedCompletionStatus`
  （Windows）唤醒 —— 无 busy-wait，空闲线程零 CPU
- **每 op 两次 io_uring_enter**（提交 + 等待）—— 这是实测中 coronet 与 asio 同机制
  差距最小的原因之一

### 3.3 内存模型：零每操作堆分配

| 对象 | 放置位置 | 说明 |
|---|---|---|
| I/O awaiter | **协程帧内**（ctx 嵌入） | 无 per-op malloc |
| task_info | promise 内 | 无独立分配 |
| IOCP operation | 对象回收池 | 完成即回收复用 |
| 协程帧 | operator new / B2 帧池 | 连接风暴场景复用 |

对比 asio：每操作 100-370B 堆分配 churn（C1000K 实测 asio 总分配 12MB > coronet 9.5MB）。

### 3.4 链式 co_await：内核级串联

`co_await (send && recv)` —— `operator&&` 构造链式操作：

- **io_uring**：`IOSQE_IO_LINK` 内核级 SQE 串联（两次 I/O 一次系统调用）
- **IOCP**：用户态 `chain_fn` 串联
- **epoll**：CRTP 类型化分发

一次 `co_await` 完成两个 I/O 后协程才恢复 —— 网络往返减半的编程模型。

### 3.5 epoll 后端的文件 I/O：线程池 + eventfd

epoll 不支持 regular file —— coronet 的 epoll 后端用**可复用线程池执行阻塞式
pread/pwrite**，完成写 eventfd 唤醒事件循环。这是与 asio 的关键差异：asio 的
epoll 后端**根本没有文件异步 I/O**（`ASIO_HAS_FILE` 仅随 IOCP/io_uring 定义），
coronet 三后端都有文件 I/O。

### 3.6 平台工厂：`detail::platform_io::make_*()`

`async_io.hpp` 中每个工厂函数体仅一行：
```cpp
return detail::platform_io::make_read(fd, buf, off);
```
平台选择集中在 `platform_io` —— 上层代码零平台分支，可读性/可维护性的关键。

---

## 四、核心优势（数据支撑）

### 4.1 性能：三端 C1000K（2026-08-02 同日实测，100 万请求 × 1000 连接）

| 服务端（单线程） | epoll | io_uring | Windows (IOCP) |
|---|---|---|---|
| coronet_ST | 47,099 | **53,743** | 47,950 |
| coronet_chain | 45,473 | **55,145** | 47,277 |
| ASIO_coro_ST | 45,253 | 49,603 | 47,210 |

- 单线程三端全部领先 asio 协程版（**+1.6% ~ +8.3%**）
- **io_uring 单线程 53.7k 全场最佳**，链式 55.1k 更高 —— 对称转移调度链式零开销
- 多线程：Windows **+72.5%**（46,999 vs 27,249）

### 4.2 单核效率：每 rps 的 CPU 成本

| 端 | coronet_MT rps/CPU% | ASIO_coro_MT rps/CPU% | 效率比 |
|---|---|---|---|
| Linux epoll | 31,787 / 70% | 36,759 / **110%** | coronet 高 ~35% |
| Linux io_uring | 33,759 / 50% | 41,085 / **110%** | coronet 高 ~70% |
| Windows | 46,999 / **26.7%** | 27,249 / 69.6% | coronet 高 **4.5×** |

asio MT 靠多核堆 CPU（线程池 spin/分发）换吞吐；coronet 对称转移 + SPSC 环
几乎无锁竞争。Linux MT 名义差距另有实证：3 × redis-benchmark 聚合 **67.3k rps
= 1.78× 扩展** —— 单进程客户端为瓶颈，非调度缺陷。

### 4.3 内存：修复后与 asio 同级

| 端 | 修复前 coronet | 修复后 coronet | asio 参照 |
|---|---|---|---|
| epoll ST | 13-15MB | **5-8MB** | 10MB |
| io_uring ST | 15MB | **7-9MB** | 10MB |
| Windows ST | 14MB | **5-6MB** | 5-7MB |

3× 差距的 100% 来自 detached 帧泄漏（P2-1，§五.2），修复归零；io_uring ring
预分配由 B1 配置化（32768 → 4096 条目，MT 35MB → 12MB）。

### 4.4 跨平台一致性

三端（epoll / io_uring / IOCP）性能曲线形状一致（ST 45-54k、MT 32-47k），
说明抽象层没有平台特化倾斜 —— 一套代码三端同水平的工程验证。

---

## 五、特别考虑的地方（设计权衡与踩坑）

### 5.1 io_uring awaiter 的时序约束（最重要的设计约束）

```
构造:  get_sq_entry() + prep(SQE)     ← SQE 提交准备
挂起:  await_suspend() 注册 handle     ← 完成事件路由
```

**约束：CQE 先于 await 到达时，null-handle 路径丢弃 CQE → 协程永久挂起。**

推论：**单协程内 I/O overlap 不可行**（不能"提交后不挂起，稍后再 await"）——
必须先挂起等结果、或借助链式/多协程。cp_tool 的"双缓冲流水"因此是严格串行
（read 完成才发 write），并实测验证了与 asio 同结构同性能 —— 这是机制约束，
不是实现缺陷，文档化以阻止后人重踩。

### 5.2 detached 帧泄漏链（P2-1）：一次修复牵出三层问题

1. **P0-2**（07-26）：MSVC when_all 死锁 → `final_suspend` 改为总是挂起 +
   awaiter 独立默认成员（永远 false）
2. **副作用**：`detach()` 清空句柄后协程在 final_suspend 挂起 → **帧孤儿化永久泄漏**
   （每连接 ~4.4KB，三端一致，C1000K 3× 内存差距的全部来源）
3. **P2-1 修复**：flag 通过**构造函数注入**（消除"默认 false 掩盖未接线"），
   `await_ready()==true` 重新启用 C++20 运行时自动销毁 —— 同时规避 MSVC
   运行时在 `await_suspend` 后访问已毁帧的 use-after-free

教训：协程 final_suspend 的"挂起/恢复/销毁"三态语义是全局契约，任何改动
必须全链路审计（detach / when_all / MSVC 运行时 / 帧池析构排空）。

### 5.3 平台栈尺寸与栈上缓冲

- Windows 默认线程栈 1MB：`io_context ctx[N]` N≥8 栈溢出 → 文档约束 N≤6，
  link 时 `/STACK:2097152` 可解除
- `reap_swap`：heap 分配的 `std::vector`（131KB）避免 8+ 上下文在栈上膨胀

### 5.4 跨线程帧回收（B2 帧池的 MT 安全性）

帧可能在线程 A 分配、线程 B 释放（跨线程 co_spawn）—— 帧池必须 thread_local
（函数局部静态，线程退出自动排空），不可用全局池（需要锁，性能归零）。

### 5.5 环境特性识别（WSL2 / 9P / 内存损坏）

- WSL2 `/mnt/c /mnt/d` 走 virtio-9p，**数据段无校验和**，单比特翻转 ~14GB/次
- 深度取证发现**固定损坏点**（offset % 4096 == 2503、bit4 翻转，Windows 原生
  路径也命中）→ 判定为物理内存/驱动坏点，非库 bug
- 工程原则：**verify 是守护网，不因偶发红而移除**；取证先行（hash 对比 +
  字节 diff + xor 签名）再定论

### 5.6 asio 对照实验中发现的第三方语义

- `scheduler::run()` 在 `outstanding_work_==0` 时自停 —— 第二次 `run()` 空转，
  asio 正确用法是单次 run + 主协程串链（cp_tool_asio 已按此模式实现）
- `ASIO_HAS_IO_URING` 是 opt-in 宏，且 epoll 后端无文件 I/O —— 文件拷贝对照
  实验 Linux 侧必须走 io_uring，Windows 侧 IOCP 天然支持

---

## 六、软件工程学思想体现

### 6.1 协程控制流优化

| 技术 | 工程思想 | 收益 |
|---|---|---|
| 对称转移调度（final 返回父句柄） | 控制流最小化跳转 | 链式场景零开销（55.1k rps 全场最高） |
| 零 wrapper 帧 co_spawn | 消除不必要的抽象层 | 每任务省一帧分配 |
| 链式 co_await → IOSQE_IO_LINK | 把协议级优化下推内核 | 网络往返减半 |
| 双缓冲 + offset-based I/O | 流水线思想 + 消除文件指针竞争 | cp_tool 三路径与 asio 持平 |

### 6.2 并发安全

| 机制 | 设计 |
|---|---|
| SPSC 无锁环（spsc_cursor） | 单生产者单消费者无锁；多生产者经跨线程队列汇聚 |
| eventfd / PQCS 唤醒 | 无 busy-wait，空闲零 CPU；唤醒一次性 |
| thread_local 帧池 | 帧 A 分配 B 释放的跨线程安全（见 §五.4） |
| IOCP deinit 三轮排空 | 捕获后台线程竞态投递（Bug #5 修复） |
| 计数器三端统一 | `requests_to_reap` 增减语义对齐（Bug #3 修复） |

### 6.3 内存安全

- **帧生命周期全链路审计**（detach → final_suspend → 自动销毁），P2-1 以构造
  注入消除未接线 flag
- MSVC 协程运行时 use-after-free → `await_ready()==true` 路径规避（Bug #1）
- `channel::buffer_end()` past-the-end 解引用 → `data()+capacity`（Bug #2）
- `iocp_operation` 回收前移，覆盖所有 early-return 路径（Bug #7，256MB 泄漏修复）
- `AcceptEx` 后 `SO_UPDATE_ACCEPT_CONTEXT` 继承属性（Bug #6）
- `std::assume_aligned` 在 MSVC LTO 下推导过强对齐 → 移除（Bug #4）

### 6.4 Modern C++ 优秀实践

| 实践 | 应用 |
|---|---|
| C++20 协程 | task / generator / channel 全栈原生协程 |
| Concepts | `operation_concept` / `proactor_concept` 编译期契约 |
| CRTP 静态多态 | 三后端零虚表 |
| RAII | `defer` / socket RAII / mutex lock_guard |
| 模板元编程 | when_all 折叠表达式、类型化链式分派 |
| 标准对齐 | generator 实现对照 P2502R2 |
| 命名协程函数约定 | 规避协程 lambda 的捕获/生命周期陷阱（asio 官方亦不推荐） |

### 6.5 工程方法（研发流程层面）

1. **数据驱动排查**：性能问题不猜 —— LD_PRELOAD malloc 探针直方图、Windows
   `_msize` 活块计数、RSS 前后对照、最小复现实验，四层交叉验证
2. **对照实验**：新增 asio 协程版服务器（同编程模型、逐行镜像结构），先证伪
   "库实现差距"再谈优化 —— 文件拷贝三路径、C1000K 三端均如此
3. **回归体系**：三端 × 36/35 项 ctest + C1000K 压测脚本化（bench_c1000k.sh/.ps1），
   任何优化必须全量回归无退化
4. **决策文档化**：每次审查/排查沉淀 CodeReview.md（含修复前后数据表）；
   实测数据留存报告文件，结论可追溯
5. **平台差异隔离**：`#ifdef` 收敛到 `platform/` 与 `detail::platform_io`，
   工厂层零分支 —— 差异是特性不是散弹
6. **估算先行**：压测实验前按数据量/速率估算时长，超时即查（避免死等卡死进程）

---

## 七、与 asio 协程版设计对比总结

| 维度 | coronet | asio | 结论 |
|---|---|---|---|
| 帧大小 | 4288-4752B | 4368B | 相当 |
| 每操作分配 | **零** | 100-370B/次 | **coronet 优** |
| 帧生命周期 | 自动销毁（P2-1 后） | 自动销毁 | 对齐 |
| 帧回收池 | thread_local 池（B2） | thread_local 池 | 已吸收 |
| 调度 | 对称转移，无 wrapper 帧 | awaitable_thread 帧栈泵 + entry_point | coronet 更省 |
| co_spawn | 直接调度 | wrapper 帧 | coronet 更省 |
| 文件 I/O（epoll 端） | 线程池 + eventfd | **无**（仅 io_uring） | coronet 优 |
| 单核效率（MT） | 高 35% - 4.5× | 多核堆 CPU | **coronet 优** |

**总体结论**：coronet 与 asio 协程版同场竞技，性能同级或更优、内存同级、
单核效率显著更优；架构上无需重构 —— 持续吸收 asio 优秀设计（帧池），
并保持零每操作分配的优势。

---

## 八、关键指标速查

- **测试矩阵**：Linux epoll/io_uring 36/36、Windows IOCP 35/35（cp_tool_asio 偶发
  受环境内存翻转影响，非代码缺陷）
- **C1000K**：coronet_ST 三端 43.3k ~ 53.7k rps，内存 6-9MB
- **文件拷贝**：Windows 原生 708 MB/s（coronet）/ 676（asio）持平；WSL 9P 链路上限
  ~110 MB/s（协议层瓶颈，两库一致）
- **构建**：Linux `-DCORONET_IOURING=ON` 切换后端；Windows 自动 IOCP
