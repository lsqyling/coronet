#pragma once

#include <coroutine>
#include <cstdint>

namespace coronet::detail {

/*
 * 每个 I/O 操作的元数据结构体。
 *
 * 这是 coronet 中最核心的数据结构之一 —— 每个提交给内核的 I/O 操作
 * 都对应一个 task_info，它串联了 I/O 请求的生命周期：
 * 提交 -> 内核处理 -> 完成回调 -> 恢复协程。
 *
 * 字段设计：
 * - handle：等待此 I/O 完成的协程句柄。当 I/O 完成时，通过此句柄恢复协程。
 * - result：I/O 操作的结果（字节数或错误码），与 POSIX read/write 等返回值一致。
 * - chain_ctx / chain_fn：用于链式 I/O（co_await (op1 && op2)）：
 *   - 在 io_uring 后端，链式操作由内核的 IOSQE_IO_LINK 完成，无需用户态回调
 *   - 在 epoll/IOCP 后端，第一个操作完成时通过 chain_fn 启动第二个操作
 *
 * user_data 编码方案（64 位）：
 * - bits [63:3]：指向 task_info 的指针（8 字节对齐，低 3 位为 0）
 * - bits [2:0] ：类型标记（当前使用 user_data_type 枚举，预留未来扩展）
 *
 * 为什么用指针编码而非间接索引表：
 * - 直接指针解码仅需一次位运算（& ~7），无需查表
 * - 没有表大小的限制，也不会因表增长而需要动态扩容
 * - task_info 的生命周期由协程的 promise 管理，指针在 I/O 完成前始终有效
 *
 * 为什么不直接用指针而保留 3 位类型标记：
 * - 区分不同类型的完成事件（普通 I/O、链式回调、内部操作等）
 * - 未来可以扩展额外的类型而不改变接口
 * - 低 3 位包含的信息可以在 completion 处理时分支到不同的处理流程
 */
/// Per-operation info: stores the awaiting coroutine handle and the I/O result.
/// 每个操作的信息：存储等待的协程句柄和 I/O 结果。
/// user_data encoding for completions:
///   - bits [63:3]: pointer to this task_info (8-byte aligned)
///   - bits [2:0] : type tag (reserved for future use)
struct task_info {
    std::coroutine_handle<> handle{nullptr};
    int32_t result{0};
    void*  chain_ctx{nullptr};               // chained: pointer to next operation
    void (*chain_fn)(void* ctx) noexcept {nullptr};  // chained: starts next I/O

    /// Encode this pointer as the user_data for SQE / OVERLAPPED
    /// 将此指针编码为 SQE / OVERLAPPED 的 user_data
    uint64_t as_user_data() const noexcept {
        return reinterpret_cast<uintptr_t>(this);
    }

    /// Decode a user_data back to task_info*
    /// 从 user_data 解码回 task_info*
    static task_info* from_user_data(uint64_t ud) noexcept {
        return reinterpret_cast<task_info*>(ud & ~uint64_t(7));
    }

    /// Extract type tag from user_data
    /// 从 user_data 提取类型标记
    static uint8_t type_tag(uint64_t ud) noexcept {
        return static_cast<uint8_t>(ud & 7);
    }
};

} // namespace coronet::detail
