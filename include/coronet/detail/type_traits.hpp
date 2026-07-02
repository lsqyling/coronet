#pragma once

#include <type_traits> // IWYU pragma: export

namespace coronet::detail {

/*
 * false_v：用于模板中触发 static_assert 延迟求值的辅助变量模板。
 *
 * 标准用法：
 *   template<typename T>
 *   void foo() {
 *       static_assert(false_v<T>, "not implemented for this type");
 *   }
 * 如果直接用 static_assert(false, ...)，在模板实例化前就会触发。
 * false_v<T> 是 dependent false，只有在实例化时才会求值。
 * 这利用了 C++ 模板的两阶段编译规则。
 */
template<typename...>
inline constexpr bool false_v = false;

/*
 * remove_rvalue_reference：移除右值引用，保留左值引用。
 *
 * 与 std::remove_reference 不同——标准库会移除所有引用类型，
 * 而此模板只移除 T&&，保留 T&。
 *
 * 用途：在完美转发场景中，有时需要区分"原始类型"和"被移动的类型"。
 * 例如，当协程等待器（awaiter）以右值传入时，可以通过此 trait
 * 保留其左值引用信息，避免在 co_await 链中丢失引用语义。
 */
template<typename T>
struct remove_rvalue_reference {
    using type = T;
};

template<typename T>
struct remove_rvalue_reference<T &&> {
    using type = T;
};

template<typename T>
using remove_rvalue_reference_t = typename remove_rvalue_reference<T>::type;

/*
 * get_awaiter_result_t：从等待器的 await_resume 返回类型推导结果类型。
 *
 * 用于在泛型代码中获取协程等待器的返回类型。
 * 例如，在 when_all / when_any 等组合器中，需要知道每个
 * 子协程的返回类型以构建正确的 Promise 类型。
 *
 * 使用 decltype 直接调用 await_resume 而非手动指定类型，
 * 可以自动适应任何符合协程规范的等待器。
 */
template<typename Awaiter>
using get_awaiter_result_t = decltype(std::declval<Awaiter>().await_resume());

} // namespace coronet::detail
