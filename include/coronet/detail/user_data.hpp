#pragma once
#include <cstdint>

namespace coronet::detail {

/*
 * io_uring 和 IOCP 的完成回调都通过 user_data 传递上下文信息。
 * user_data 是一个 64 位整数，编码了操作类型和指向 task_info 的指针。
 *
 * 为什么用位编码而非直接存指针：
 * - 指针需要 8 字节对齐（低 3 位始终为 0），这些空闲位可以携带额外信息
 * - 通过在低 3 位存储类型标记，可以在 completion 处理时区分不同类型的操作
 * - 无需额外的映射表或动态类型识别，直接通过位运算解码
 *
 * 预留值用于无上下文的内部操作（如 NOP、唤醒通知），
 * 这些操作不需要关联到某个具体的 I/O 请求。
 */
/// Reserved user_data values for internal operations.
/// 预留给内部操作使用的 user_data 值，不关联到具体的 I/O 操作。
enum class reserved_user_data : uint64_t {
    nop,   /// 空操作标记 / No-operation marker
    none   /// 无操作 / No associated operation
};

/*
 * user_data_type 枚举存储在 user_data 的低 3 位（bits [2:0]），
 * 用于在 completion 阶段快速判断操作类型。
 *
 * 当前支持的编码：
 * - task_info_ptr：低 3 位清零后为 task_info 指针
 *   这是最常见的类型——几乎所有 I/O 操作都关联到一个 task_info
 * - none：无类型标记（用于预留值）
 *
 * 为什么预留了 3 位（8 种类型）：
 * - 最常用的是指针类型，一种就够了
 * - 其他类型可以为未来的优化预留，例如：批处理标记、特殊回调类型等
 * - 3 位编码使掩码对齐到 8 字节，便于指针操作
 */
/// Type tag stored in bits [2:0] of user_data.
enum class user_data_type : uint8_t {
    task_info_ptr,  /// 指向 task_info 结构体的指针 / Pointer to task_info
    none            /// 无类型 / No type
};

static_assert(uint8_t(user_data_type::none) <= 8);

} // namespace coronet::detail
