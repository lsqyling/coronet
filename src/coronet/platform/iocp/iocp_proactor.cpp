#include "coronet/platform/iocp/iocp_proactor.hpp"
#include "coronet/log/log.hpp"


#include <cstdio>
#include <cstdlib>

namespace coronet::platform::iocp {

// ---- iocp_operation ----
// ---- iocp_operation 实现 ----

void iocp_operation::on_pending(iocp_proactor* proactor) {
    // 处理真正异步等待的 I/O 操作（返回 WSA_IO_PENDING 的情况）。
    // 操作已经提交给内核，内核完成后会自动将 OVERLAPPED 投递到 IOCP。
    // 但我们需要检测"操作是否已经同步完成"（ready_ 已被设置）：
    // 如果有另一条路径（如完成回调）已经设置了 ready_，我们需要手动 post 结果。
    if (InterlockedCompareExchange(&ready_, 1, 0) != 0) {
        proactor->post_completion(this, static_cast<DWORD>(InternalHigh), 0);
    }
}

void iocp_operation::on_sync_completion(iocp_proactor* proactor, DWORD bytes) {
    // 处理同步完成的 I/O 操作。
    // 某些操作（如 closesocket、shutdown）没有真正的异步形式，只能同步执行。
    // 我们需要手动设置 OVERLAPPED 的 Internal 字段（错误码）和 InternalHigh 字段（传输字节数），
    // 然后将操作 post 到 IOCP，使协程能通过统一的 wait_completion 路径恢复。
    Internal = 0;       // 成功
    InternalHigh = bytes;
    ready_ = 1;
    proactor->post_completion(this, bytes, 0);
}

// ---- Per-thread operation recycling (ASIO pattern) ----
// ---- 线程本地操作回收（ASIO 模式） ----
//
// 关键优化：避免高频 I/O 场景下的堆分配开销。
//   - 线程本地空闲链表（thread_local op_free_list）：每个线程维护一个已回收 operation 的链表。
//   - acquire_operation() 优先从空闲链表获取，链表为空时才 new。
//   - 操作完成后通过 recycle_operation() 归还到空闲链表。
//   - max_count = 128 限制链表大小，防止闲置 operation 占用过多内存。
//   - 链表通过 iocp_operation::Internal 字段（ULONG_PTR）串联节点：
//     Internal 在 OVERLAPPED 中原本用于存储错误码，但在回收状态下可安全复用。

namespace {

struct op_free_list {
    iocp_operation* head = nullptr;
    int count = 0;
    static constexpr int max_count = 128;
};

thread_local op_free_list tl_ops;

} // anonymous namespace

std::unique_ptr<iocp_operation> iocp_proactor::acquire_operation() {
    auto& fl = tl_ops;
    if (fl.head) {
        // 从空闲链表头部取出一个 operation 复用
        auto* op = fl.head;
        fl.head = reinterpret_cast<iocp_operation*>(op->Internal);
        // 链表节点通过 Internal 字段串联：Internal 存储下一个节点的指针
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
        raw->Internal = reinterpret_cast<ULONG_PTR>(fl.head);
        // 在回收状态下，Internal 字段用于存储链表 next 指针
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
    if (iocp_handle_) {
        // 先发送一个特殊的退出信号（key=1, overlapped=nullptr）
        PostQueuedCompletionStatus(iocp_handle_, 0, 1, nullptr);
    }
    // 排空 IOCP 队列中所有待处理的完成事件
    // 循环读取直到没有更多事件（key=1 是退出信号，会跳过）
    while (true) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* ov = nullptr;
        BOOL ok = GetQueuedCompletionStatus(iocp_handle_, &bytes, &key, &ov, 0);
        if (!ok && !ov) break;
        if (key == 1 && !ov) continue;
        if (ov) {
            // 释放未完成的操作
            std::unique_ptr<iocp_operation> op{iocp_operation::from_overlapped(ov)};
        }
    }
    if (iocp_handle_) {
        CloseHandle(iocp_handle_);
        iocp_handle_ = nullptr;
    }
    outstanding_work_.store(0, std::memory_order_release);
}

int iocp_proactor::submit(bool /*wait*/) noexcept {
    // IOCP 不需要 submit：I/O 操作在调用 WSASend/WSARecv/... 时直接提交给内核。
    // 这与 io_uring 不同（io_uring 需要调用 io_uring_enter syscall 提交 SQ ring）。
    // 因此 submit() 始终返回 0，仅用于满足 proactor_concept 接口兼容性。
    return 0;
}

int iocp_proactor::wait_completion(completion_info* info) noexcept {
    // wait_completion 的核心实现：调用 GetQueuedCompletionStatus 阻塞等待完成事件。
    //
    // 流程：
    //   1. GQCS 阻塞直到有完成事件到达（INFINITE 超时）
    //   2. 如果返回的 overlapped 为 nullptr，说明是 wakeup 信号（key=1），返回 0
    //   3. 从 overlapped 转换回 iocp_operation，提取结果
    //   4. 填充 completion_info 并返回 1
    //
    // 注意：overlapped != nullptr 但 op->is_ready() 为 false 的情况表示操作尚未完成，
    // 这可能发生在 IOCP 被某些伪事件唤醒时。
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* overlapped = nullptr;
    BOOL ok = GetQueuedCompletionStatus(
        iocp_handle_, &bytes, &key, &overlapped, INFINITE);
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
    // 用于以下场景：
    //   1. 同步完成的 I/O 操作（如 closesocket）需要手动通知协程
    //   2. on_pending 检测到操作已经同步完成后的补救投递
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
