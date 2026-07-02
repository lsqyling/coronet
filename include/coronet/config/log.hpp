#pragma once

#include <cstdint>

namespace coronet::config {

/*
 * 日志级别枚举，按严重程度递增排列。
 * 使用 uint8_t 作为底层类型以节省空间——该枚举实例可能出现在
 * 多个编译单元中，紧凑的表示有助于减少二进制体积。
 *
 * verbose -> debug -> info -> warning -> error -> no_log
 *
 * verbose 是最详细的追踪级别（打印每个 I/O 操作的提交和完成），
 * 仅在深度调试时使用。
 * no_log 完全关闭所有日志输出，连错误消息也不会打印。
 */
enum class log_level : uint8_t { verbose = 0, debug = 1, info = 2, warning = 3, error = 4, no_log = 5 };

// Production
/*
 * 生产环境默认日志级别设为 warning，意味着只输出警告和错误。
 * 这是性能与可诊断性的权衡：
 * - 生产环境中过多的日志会拖慢 IOPS，特别是 verbose 级别会在
 *   每次 I/O 操作时产生输出，导致严重的性能退化。
 * - 保留 warning/error 级别可以捕捉到异常情况，便于问题排查。
 *
 * 如需调试，在构建时修改此值即可（或者通过构建系统传入宏定义覆盖）。
 */
inline constexpr log_level level = log_level::warning;

} // namespace coronet::config
