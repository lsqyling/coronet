#pragma once

#include <type_traits>

namespace coronet::detail {

/*
 * RAII 锁守卫 —— 对任何有 unlock() 方法的锁类型进行 RAII 封装。
 *
 * 为什么需要这个类（而不是直接用 std::lock_guard）：
 * - 标准库的 std::lock_guard 模板期望 lock()/unlock() 符合 BasicLockable 要求，
 *   在构造时会调用 lock()。但我们的 spinlock 在某些场景下需要提前手动锁定
 *   （例如在 I/O 提交路径中，某些锁必须在特定执行点提前锁定）。
 * - 本 lock_guard 假设锁已经由调用者锁定，析构时仅调用 unlock()。
 *   这提供了更灵活的使用模式："手动锁定，自动释放"。
 *
 * 典型使用场景：
 * - 在多个步骤中保护跨步操作（例如 seq-lock 模式）
 * - 需要在持有锁时执行一些非平凡的初始化
 * - 锁获取和释放之间存在复杂的控制流（如循环、条件分支）
 *
 * 为什么删除移动语义：
 * - RAII 守卫持有对锁的引用，移动后会导致析构时解锁错误的对象
 * - 移动一个"即将析构的资源守卫"通常意味着设计错误
 * - 这是一个不可移动、不可复制的守卫，与 std::lock_guard 一致
 *
 * 为什么禁止 Lockable&：
 * - 引用类型作为模板参数会导致奇怪的语义
 * - static_assert 确保用户传递的是值类型，如 spinlock 而非 spinlock&
 * - 如果确实需要引用语义，可以用指针或 std::ref
 */
/// RAII lock guard for any type with unlock() method.
/// NOTE: the mutex must already be locked before constructing this guard.
/// 注意：构造此守卫时，锁必须已经被锁定。
template<typename Lockable>
class lock_guard {
    static_assert(!std::is_reference_v<Lockable>,
                  "lock_guard<Lockable&> is not allowed; store a pointer or use std::ref");

    Lockable* lock_;

public:
    /// Construct a guard for an already-locked mutex
    /// 为一个已经锁定的互斥量构造守卫
    constexpr explicit lock_guard(Lockable& lk) noexcept
        : lock_(std::addressof(lk)) {}

    constexpr ~lock_guard() noexcept {
        if (lock_) lock_->unlock();
    }

    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;
    lock_guard(lock_guard&&) = delete;
    lock_guard& operator=(lock_guard&&) = delete;
};

} // namespace coronet::detail
