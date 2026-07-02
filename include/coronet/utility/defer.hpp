/// RAII defer: executes lambda on scope exit. Aligned with co_context.
#pragma once

namespace coronet {

/*
 * RAII defer —— 作用域退出时自动执行 lambda。
 *
 * 设计原理：
 * - 继承传入的 Lambda 类型（空基类优化，EBCO），
 *   避免额外存储 Lambda 对象实例带来的内存开销。
 *   如果 Lambda 是无状态的，继承将占用零额外空间。
 * - 析构函数调用 Lambda::operator()()，确保无论是因为
 *   正常流程、异常、还是 early return，defer 都能正确执行。
 *
 * 典型用途：
 * - 在 I/O 操作前后设置和恢复状态
 * - 在获取锁后确保释放锁（配合 spinlock）
 * - 确保在协程挂起前提交未完成的 I/O 请求
 *
 * 与 co_context 保持一致的 API 设计，便于两个项目间的代码迁移。
 */
template<typename Lambda>
struct defer : Lambda {
    ~defer() { Lambda::operator()(); }
};

/*
 * C++17 CTAD（类模板参数推导）指引。
 * 允许 defer{[]{ ... }} 而不是 defer<lambda_type>{[]{ ... }}。
 * 这让使用语法更简洁——用户只需写 defer{ ... } 即可。
 */
template<typename Lambda>
defer(Lambda) -> defer<Lambda>;

} // namespace coronet
