#include "coronet/platform/iocp/iocp_proactor.hpp"
#include "coronet/log/log.hpp"


#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace coronet::platform::iocp {

// ---- iocp_operation ----
// ---- iocp_operation 实现 ----

void iocp_operation::on_pending(HANDLE iocp) noexcept {
    // 处理真正异步等待的 I/O 操作（返回 WSA_IO_PENDING 的情况）。
    // 操作已经提交给内核，内核完成后会自动将 OVERLAPPED 投递到 IOCP。
    // 但我们需要检测"操作是否已经同步完成"（ready_ 已被设置）：
    // 如果有另一条路径（如完成回调）已经设置了 ready_，我们需要手动 post 结果。
    //
    // P1-2 fix: 接收 HANDLE（内核句柄）而非 iocp_proactor*（内存指针）。
    // HANDLE 是值语义，即使 io_context 已析构，PostQueuedCompletionStatus
    // 对已关闭的 handle 返回 0（ERROR_INVALID_HANDLE），是良定义行为。
    if (InterlockedCompareExchange(&ready_, 1, 0) != 0) {
        if (!PostQueuedCompletionStatus(iocp,
                static_cast<DWORD>(InternalHigh), 0,
                static_cast<OVERLAPPED*>(this))) {
            // IOCP 已关闭 — 安全回收
            recycle_operation(std::unique_ptr<iocp_operation>{this});
        }
    }
}

void iocp_operation::on_sync_completion(HANDLE iocp, DWORD bytes) noexcept {
    // 处理同步完成的 I/O 操作。
    // 某些操作（如 closesocket、shutdown、后台线程池 Sleep/_read/_write）
    // 没有真正的异步形式，只能同步执行。
    // 我们需要手动设置 OVERLAPPED 的 Internal/InternalHigh 字段，
    // 然后将操作 post 到 IOCP，使协程能通过统一的 wait_completion 路径恢复。
    //
    // P1-2 fix: 接收 HANDLE 而非 iocp_proactor*。
    // 彻底消除后台线程池 lambda 捕获 proactor 裸指针的 use-after-free 风险。
    // HANDLE 是内核对象句柄（值语义），io_context 析构后使用是安全的。
    Internal = 0;       // 成功
    InternalHigh = bytes;
    ready_ = 1;
    if (!PostQueuedCompletionStatus(iocp, bytes, 0,
            static_cast<OVERLAPPED*>(this))) {
        // IOCP 已关闭 — 安全回收，避免泄漏
        recycle_operation(std::unique_ptr<iocp_operation>{this});
    }
}

// ---- Per-thread operation recycling (ASIO pattern) ----
// ---- 线程本地操作回收（ASIO 模式） ----
//
// 关键优化：避免高频 I/O 场景下的堆分配开销。
//   - 线程本地空闲链表（thread_local op_free_list）：每个线程维护一个已回收 operation 的链表。
//   - acquire_operation() 优先从空闲链表获取，链表为空时才 new。
//   - 操作完成后通过 recycle_operation() 归还到空闲链表。
//   - max_count = 128 限制链表大小，防止闲置 operation 占用过多内存。
//   - P1-3: 链表通过 iocp_operation::free_next_ 专用成员串联节点，
//     不再复用 OVERLAPPED::Internal 字段（Internal 在正常 I/O 中存储 NTSTATUS，
//     复用它在 double-free 时会静默损坏链表）。

namespace {

struct op_free_list {
    iocp_operation* head = nullptr;
    int count = 0;
    // 池上限 128 → 4096（C1000k 内存优化）：
    // 1000 并发压测下 in-flight operations 峰值 ~2000+（每连接 recv+send 各 1），
    // 旧上限 128 导致池恒满、每次 I/O 都 new/delete 一个 64B 对象
    // （650k req ≈ 1.3M 次堆分配）。高频小分配 + 5KB 协程帧的分配/释放交错，
    // 使 LFH 堆的 64KB subsegment 碎片化并永久占用 → 压测后 WS 膨胀至 ASIO 的 3 倍
    // （!heap -l 证实无泄漏，活跃块仅 480 个 0MB，是段不收缩）。
    // 4096 × 64B ≈ 256KB/线程 常驻，换取压测中零堆分配。
    static constexpr int max_count = 4096;
};

thread_local op_free_list tl_ops;

} // anonymous namespace

std::unique_ptr<iocp_operation> iocp_proactor::acquire_operation() {
    auto& fl = tl_ops;
    if (fl.head) {
        // 从空闲链表头部取出一个 operation 复用
        auto* op = fl.head;
        fl.head = op->free_next_;  // P1-3: 使用专用 free_next_ 成员
        --fl.count;
        op->reset_for_reuse();
        // reset_for_reuse 重建 OVERLAPPED 子对象并清空所有扩展字段
        return std::unique_ptr<iocp_operation>{op};
    }
    return std::make_unique<iocp_operation>();
}

void recycle_operation(std::unique_ptr<iocp_operation> op) noexcept {
    if (!op) return;
    auto& fl = tl_ops;
    if (fl.count < op_free_list::max_count) {
        // 将 operation 归还到空闲链表头部
        auto* raw = op.release();
        raw->free_next_ = fl.head;  // P1-3: 使用专用 free_next_ 成员
        fl.head = raw;
        ++fl.count;
    }
    // 如果链表已满（>= max_count），unique_ptr 析构时自动 delete
}

// ---- iocp_proactor ----
// ---- iocp_proactor 实现 ----

void iocp_proactor::init(uint32_t entries) {
    entries_ = entries;
    // CreateIoCompletionPort 创建一个新的 IOCP 内核对象。
    // 参数：INVALID_HANDLE_VALUE 表示新建一个不与任何文件关联的 IOCP，
    // 后续通过 register_handle() 将 socket handle 关联到这个 IOCP。
    // 最后一个参数 0 表示允许任意数量的并发线程处理完成事件（由内核调度）。
    iocp_handle_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (!iocp_handle_) {
        log::d("iocp_proactor: CreateIoCompletionPort failed\n");
        std::abort();
    }
}

void iocp_proactor::deinit() noexcept {
    if (!iocp_handle_) return;

    // 先发送一个特殊的退出信号（key=1, overlapped=nullptr）
    PostQueuedCompletionStatus(iocp_handle_, 0, 1, nullptr);

    // P2-3: 使用 outstanding_work_ 原子计数器替代脆弱的三轮启发式排空。
    // outstanding_work_ 在 await_suspend 中递增（work_started），
    // 在 handle_completion 中递减（work_finished）。
    // 当 outstanding_work_ 降为 0 时，所有 I/O 操作都已完成，
    // 可以安全关闭 IOCP handle。
    //
    // 排空循环：
    //   - 使用 timeout=0 非阻塞排空已完成的操作
    //   - 如果 outstanding_work_ > 0，说明有后台线程仍在执行阻塞操作
    //     （win_timeout/win_read/win_write），等待它们完成并投递结果
    //   - 使用短超时（1ms）等待，避免无限阻塞但给后台线程足够时间
    //   - 最多等待 1 秒（墙钟时间），防止永久阻塞
    //
    // P2-x fix: 排空预算改为按墙钟时间限制，而非轮次 × GQCS(1ms)。
    // GetQueuedCompletionStatus 的 1ms 超时实际耗时受系统定时器粒度影响
    // （默认 ~10-15.6ms/轮），原 1000 轮上限实际耗时可达 ~10s，
    // 远超"最多 1 秒"的设计意图 —— combinator_stress 每阶段 io_context
    // 析构耗时 ~10s 的根因。超时预算未用完的轮次会正常收割后续完成事件，
    // 未在预算内完成的超时操作会在 IOCP 关闭后投递失败并被安全回收。
    const auto drain_deadline = std::chrono::steady_clock::now()
                              + std::chrono::seconds(1);
    int drain_rounds = 0;
    constexpr int max_drain_rounds = 1000;  // 兜底上限，正常情况下不会触达
    while (outstanding_work_.load(std::memory_order_acquire) > 0 &&
           drain_rounds < max_drain_rounds &&
           std::chrono::steady_clock::now() < drain_deadline) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* ov = nullptr;
        // 首轮非阻塞，后续轮次 1ms 超时等待后台线程投递
        DWORD timeout = (drain_rounds == 0) ? 0 : 1;
        BOOL ok = GetQueuedCompletionStatus(iocp_handle_, &bytes, &key, &ov, timeout);
        if (!ok && !ov) {
            // 超时且无完成事件
            drain_rounds++;
            continue;
        }
        if (key == 1 && !ov) continue;  // wakeup signal, skip
        if (ov) {
            // 回收操作对象，递减 outstanding_work_（通过 handle_completion 路径）
            // 但 deinit 路径不经过 handle_completion，所以手动回收 + 递减
            std::unique_ptr<iocp_operation> op{
                iocp_operation::from_overlapped(ov)};
            if (outstanding_work_.load(std::memory_order_acquire) > 0) {
                outstanding_work_.fetch_sub(1, std::memory_order_release);
            }
        }
        drain_rounds++;
    }

    // 最终非阻塞排空：处理可能在计数器归零后投递的残留事件
    while (true) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* ov = nullptr;
        BOOL ok = GetQueuedCompletionStatus(iocp_handle_, &bytes, &key, &ov, 0);
        if (!ok && !ov) break;
        if (key == 1 && !ov) continue;
        if (ov) {
            std::unique_ptr<iocp_operation> op{
                iocp_operation::from_overlapped(ov)};
        }
    }

    if (outstanding_work_.load(std::memory_order_acquire) > 0) {
        log::w("[iocp] deinit: %lld outstanding operations after drain\n",
               (long long)outstanding_work_.load(std::memory_order_acquire));
    }

    CloseHandle(iocp_handle_);
    iocp_handle_ = nullptr;
    outstanding_work_.store(0, std::memory_order_release);
}

int iocp_proactor::submit(bool /*wait*/) noexcept {
    // IOCP 不需要 submit：I/O 操作在调用 WSASend/WSARecv/... 时直接提交给内核。
    // 这与 io_uring 不同（io_uring 需要调用 io_uring_enter syscall 提交 SQ ring）。
    // 因此 submit() 始终返回 0，仅用于满足 proactor_concept 接口兼容性。
    return 0;
}

int iocp_proactor::wait_completion(completion_info* info, bool nonblocking) noexcept {
    // wait_completion 的核心实现：调用 GetQueuedCompletionStatus 阻塞等待完成事件。
    //
    // 流程：
    //   1. GQCS 阻塞直到有完成事件到达（INFINITE 超时）
    //      nonblocking=true 时使用 timeout=0 不阻塞
    //   2. 如果返回的 overlapped 为 nullptr，说明是 wakeup 信号（key=1），返回 0
    //   3. 从 overlapped 转换回 iocp_operation，提取结果
    //   4. 填充 completion_info 并返回 1
    //
    // 注意：overlapped != nullptr 但 op->is_ready() 为 false 的情况表示操作尚未完成，
    // 这可能发生在 IOCP 被某些伪事件唤醒时。
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* overlapped = nullptr;
    DWORD timeout = nonblocking ? 0 : INFINITE;
    BOOL ok = GetQueuedCompletionStatus(
        iocp_handle_, &bytes, &key, &overlapped, timeout);
    if (!overlapped) {
        // key=1 表示 wakeup/退出信号，没有实际 I/O 完成
        if (key == 1) return 0;
        return 0;
    }
    auto* op = iocp_operation::from_overlapped(overlapped);
    if (op) {
        if (!op->is_ready()) return 0;
        info->user_data = op->get_user_data();
        info->opaque = op;
    } else {
        info->user_data = static_cast<uint64_t>(key);
        info->opaque = nullptr;
    }
    // ok=true：同步或异步成功，bytes 是传输的字节数
    // ok=false：操作失败，通过 GetLastError() 获取错误码
    info->result = ok ? static_cast<int32_t>(bytes)
                      : -static_cast<int32_t>(::GetLastError());
    info->flags = ok ? 0 : 1;
    return 1;
}

intptr_t iocp_proactor::native_handle() const noexcept {
    return reinterpret_cast<intptr_t>(iocp_handle_);
}

void iocp_proactor::wakeup() noexcept {
    // 跨线程唤醒：通过 PostQueuedCompletionStatus 向 IOCP 投递一个伪事件。
    // GQCS 收到这个事件后会返回（overlapped=nullptr, key=1），
    // 通知事件循环排空跨线程协程队列。
    if (iocp_handle_) {
        PostQueuedCompletionStatus(iocp_handle_, 0, 1, nullptr);
    }
}

void iocp_proactor::post_completion(iocp_operation* op, DWORD bytes, DWORD key) {
    // 手动向 IOCP 投递一个完成事件。
    // 仅从事件循环线程调用（finish_issue 路径），proactor 保证存活。
    // 后台线程池路径已改为直接调用 on_sync_completion(HANDLE)，不再经过此方法。
    PostQueuedCompletionStatus(
        reinterpret_cast<HANDLE>(iocp_handle_), bytes, key,
        static_cast<OVERLAPPED*>(op));
}

void iocp_proactor::register_handle(uintptr_t sock) noexcept {
    // 将 socket 关联到 IOCP。
    // CreateIoCompletionPort 的第二个参数是已有的 IOCP handle，
    // 这样该 socket 上所有异步操作的完成事件都会投递到这个 IOCP。
    // 这是 IOCP 的基本使用模式：所有 socket 关联到同一个 IOCP，
    // 工作线程从这个 IOCP 中取出完成事件。
    CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(sock),
        reinterpret_cast<HANDLE>(iocp_handle_), 0, 0);
}

int iocp_proactor::poll_completions_impl(
    void* ctx, void (*callback_fn)(void*, const completion_info*)) noexcept
{
    // 批量完成收割的简化实现：每次只处理一个完成事件。
    // 与 io_uring 版本不同，IOCP 版本没有 peek 多个 CQE 的能力，
    // GQCS 每次只返回一个 OVERLAPPED。
    // 真正的"批量"需要事件循环多次调用此函数。
    completion_info info{};
    int ret = wait_completion(&info);
    if (ret > 0) callback_fn(ctx, &info);
    return ret;
}

} // namespace coronet::platform::iocp
