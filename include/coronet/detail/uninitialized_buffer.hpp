#pragma once

#include <type_traits>
#include <memory>

namespace coronet::detail {

/*
 * uninitialized_buffer —— 未初始化的类型安全存储。
 *
 * 为什么需要这个类型：
 * - C++ 的 std::optional<T> 和 std::variant<T> 在构造时会默认初始化 T，
 *   这在某些场景下是不必要的开销（例如 T 很大且需要延迟构造）。
 * - 在协程的 promise_type 中，协程结果值可能需要延迟构造（在 await_resume 时
 *   才计算），用 uninitialized_buffer 可以避免先默认构造再赋值的双重开销。
 * - 也用于 I/O 操作的结果缓冲区，在内核填充数据前不需要初始化。
 *
 * 设计权衡：
 * - 与直接使用 std::byte[] 相比，提供了类型安全的 ptr()/ref() 接口
 * - 与 std::aligned_storage 相比，更简洁且避免了已弃用的 API
 * - 不管理生命周期——调用者必须通过 construct()/destroy() 手动管理
 * - 这给了用户完全的控制权，但同时也意味着更高的使用责任
 *
 * 使用模式：
 *   uninitialized_buffer<MyType> buf;
 *   MyType* p = buf.construct(args...);   // placement new
 *   // ... use *p ...
 *   buf.destroy();                         // explicit destructor call
 */
/// A storage wrapper that holds space for a T without constructing or destroying it.
template<typename T>
struct uninitialized_buffer {
    alignas(T) std::byte storage[sizeof(T)];

    T* ptr() noexcept { return reinterpret_cast<T*>(&storage); }
    const T* ptr() const noexcept { return reinterpret_cast<const T*>(&storage); }

    T& ref() noexcept { return *ptr(); }
    const T& ref() const noexcept { return *ptr(); }

    template<typename... Args>
    T* construct(Args&&... args) {
        return std::construct_at(ptr(), std::forward<Args>(args)...);
    }

    void destroy() noexcept {
        std::destroy_at(ptr());
    }
};

} // namespace coronet::detail
