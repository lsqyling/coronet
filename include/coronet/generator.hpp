////////////////////////////////////////////////////////////////
// Reference implementation of std::generator proposal P2502R2
// Authors: Casey Carter, Lewis Baker, Corentin Jabot.
// https://godbolt.org/z/5hcaPcfvP
//
/*
 * std::generator 提案 P2502R2 的参考实现。
 *
 * generator 是一个协程返回类型，产生一系列值（类似 Python 的 yield 或
 * C# 的 IEnumerable），支持按需惰性求值（lazy evaluation）。
 *
 * 为什么在异步 I/O 库中包含 generator：
 * - generator 本身是同步的，但它与协程机制共享相同的底层基础设施
 *   （promise_type、awaitable、coroutine_handle 等）
 * - 在异步编程中，generator 可用于"流式处理"：将一个 I/O 操作的
 *   多次结果以同步风格逐个产生，上层以 for-range 循环消费
 * - 例如：逐行读取文件、分批处理网络数据等
 * - 这是 C++23 标准库的一部分，提供参考实现以便测试和过渡
 *
 * 与 task<T> 的区别：
 * - task<T> 产生一个最终值，generator<T> 产生一个值序列
 * - task<T> 是 eager（在 co_await 时执行），generator 是 lazy
 *   （在 begin()/operator++ 时推进）
 * - task<T> 有 unique ownership，generator 允许多次迭代
 *
 * 本文件是提案的标准参考实现，略有调整以适配 coronet 的命名空间。
 * 一些变量名（如 _Val、_Ptr、_Coro）遵循 MSVC STL 的内部命名风格。
 */
#ifndef CORONET_GENERATOR_HPP
#define CORONET_GENERATOR_HPP

#include <algorithm>
#include <cassert>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <ranges>
#include <type_traits>
#include <utility>

/*
 * 平台相关的宏定义：
 * - EMPTY_BASES（MSVC __declspec(empty_bases)）：优化空基类的布局，
 *     使空基类不占用额外空间，这是 MSVC 的 COM 兼容性需求。
 * - NO_UNIQUE_ADDRESS：在 MSVC 上用 [[msvc::no_unique_address]]，
 *     在 Clang/GCC 上用标准 [[no_unique_address]]。
 *     因为 MSVC 的 [[no_unique_address]] 实现不完整。
 */
#ifdef _MSC_VER
#define EMPTY_BASES __declspec(empty_bases)
#ifdef __clang__
#define NO_UNIQUE_ADDRESS
#else
#define NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#endif
#else
#define EMPTY_BASES
#define NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

// NOLINTBEGIN
namespace coronet {

/*
 * _Aligned_block：按 STPCPP_DEFAULT_NEW_ALIGNMENT（通常是 16）对齐的块。
 * 供分配器使用，确保协程帧对齐到默认对齐要求。
 */
struct alignas(__STDCPP_DEFAULT_NEW_ALIGNMENT__) _Aligned_block {
    unsigned char _Pad[__STDCPP_DEFAULT_NEW_ALIGNMENT__];
};

template<class _Alloc>
using _Rebind = typename std::allocator_traits<_Alloc>::template rebind_alloc<
    _Aligned_block>;

/*
 * _Has_real_pointers：检查分配器是否使用真实指针。
 * 协程帧的分配需要真实的连续内存，不能使用 fancy pointer（如偏移量指针）。
 * 如果分配器是 void（使用默认的 operator new/delete），或者分配器的
 * pointer 类型是原生指针，则满足要求。
 */
template<class _Alloc>
concept _Has_real_pointers =
    std::same_as<_Alloc, void>
    || std::is_pointer_v<typename std::allocator_traits<_Alloc>::pointer>;

/*
 * _Promise_allocator：协程 promise 的分配器管理。
 *
 * 协程帧的内存分配是通过 promise_type 的 operator new 完成的。
 * 标准协程支持通过 std::allocator_arg 传递自定义分配器，但分配器的
 * 生命周期管理比较复杂：
 *
 * 分配器分类：
 * 1. 无状态分配器（is_always_equal）：不需要存储，在 operator delete 中
 *    重新构造一个即可。
 * 2. 有状态分配器：必须在协程帧中存储一份拷贝，以便在销毁时使用正确
 *    的分配器释放内存。
 *
 * 类型擦除版本（_Promise_allocator<void>）：
 * - 当分配器类型在编译期未知时使用（例如通过模板推导）。
 * - 使用函数指针 _Dealloc_fn 存储释放逻辑。
 * - 分配器数据嵌入在协程帧的尾部，operator new 会分配额外空间来存储。
 * - 这种设计增加了每次分配的开销（额外的函数指针调用），但提供了
 *   最大的灵活性。
 *
 * 为什么不在 _Promise_allocator<void> 中使用 std::function：
 * - std::function 可能涉及堆分配，在协程帧分配器内部使用堆分配
 *   会导致循环依赖。
 * - 裸函数指针 + memcpy 是最轻量的类型擦除方案。
 */
template<class _Allocator = void>
class _Promise_allocator { // statically specified allocator type
  private:
    using _Alloc = _Rebind<_Allocator>;

    static void *_Allocate(_Alloc _Al, const size_t _Size) {
        if constexpr (std::default_initializable<_Alloc> && std::allocator_traits<_Alloc>::is_always_equal::value) {
            // do not store stateless allocator
            // 不存储无状态分配器
            const size_t _Count =
                (_Size + sizeof(_Aligned_block) - 1) / sizeof(_Aligned_block);
            return _Al.allocate(_Count);
        } else {
            // store stateful allocator
            // 存储有状态分配器
            static constexpr size_t _Align =
                (::std::max)(alignof(_Alloc), sizeof(_Aligned_block));
            const size_t _Count =
                (_Size + sizeof(_Alloc) + _Align - 1) / sizeof(_Aligned_block);
            void *const _Ptr = _Al.allocate(_Count);
            const auto _Al_address = (reinterpret_cast<uintptr_t>(_Ptr) + _Size
                                      + alignof(_Alloc) - 1)
                                     & ~(alignof(_Alloc) - 1);
            ::new (reinterpret_cast<void *>(_Al_address))
                _Alloc(::std::move(_Al));
            return _Ptr;
        }
    }

  public:
    static void *operator new(const size_t _Size)
        requires std::default_initializable<_Alloc>
    {
        return _Allocate(_Alloc{}, _Size);
    }

    template<class _Alloc2, class... _Args>
        requires std::convertible_to<const _Alloc2 &, _Allocator>
    static void *operator new(
        const size_t _Size,
        std::allocator_arg_t,
        const _Alloc2 &_Al,
        const _Args &...
    ) {
        return _Allocate(
            static_cast<_Alloc>(static_cast<_Allocator>(_Al)), _Size
        );
    }

    template<class _This, class _Alloc2, class... _Args>
        requires std::convertible_to<const _Alloc2 &, _Allocator>
    static void *operator new(
        const size_t _Size,
        const _This &,
        std::allocator_arg_t,
        const _Alloc2 &_Al,
        const _Args &...
    ) {
        return _Allocate(
            static_cast<_Alloc>(static_cast<_Allocator>(_Al)), _Size
        );
    }

    static void operator delete(void *const _Ptr, const size_t _Size) noexcept {
        if constexpr (std::default_initializable<_Alloc> && std::allocator_traits<_Alloc>::is_always_equal::value) {
            // make stateless allocator
            // 重新构造无状态分配器
            _Alloc _Al{};
            const size_t _Count =
                (_Size + sizeof(_Aligned_block) - 1) / sizeof(_Aligned_block);
            _Al.deallocate(static_cast<_Aligned_block *>(_Ptr), _Count);
        } else {
            // retrieve stateful allocator
            // 取回存储的有状态分配器
            const auto _Al_address = (reinterpret_cast<uintptr_t>(_Ptr) + _Size
                                      + alignof(_Alloc) - 1)
                                     & ~(alignof(_Alloc) - 1);
            auto &_Stored_al = *reinterpret_cast<_Alloc *>(_Al_address);
            _Alloc _Al{::std::move(_Stored_al)};
            _Stored_al.~_Alloc();

            static constexpr size_t _Align =
                (::std::max)(alignof(_Alloc), sizeof(_Aligned_block));
            const size_t _Count =
                (_Size + sizeof(_Alloc) + _Align - 1) / sizeof(_Aligned_block);
            _Al.deallocate(static_cast<_Aligned_block *>(_Ptr), _Count);
        }
    }
};

/*
 * _Promise_allocator<void> 特化：类型擦除的分配器管理。
 *
 * 当分配器类型在编译期未知时使用。通过在协程帧尾部存储
 * 一个函数指针（_Dealloc_fn）和（可选的）分配器实例来实现类型擦除。
 *
 * 内存布局（无状态分配器）：
 *   [协程帧数据] [_Dealloc_fn]
 *
 * 内存布局（有状态分配器）：
 *   [协程帧数据] [_Dealloc_fn] [_Alloc 实例]
 *
 * _Dealloc_fn 在分配时被 memcpy 存入，在释放时被取出调用。
 * 这避免了虚函数表或 std::function 的开销。
 */
template<>
class _Promise_allocator<void> { // type-erased allocator
  private:
    using _Dealloc_fn = void (*)(void *, size_t);

    template<class _ProtoAlloc>
    static void *_Allocate(const _ProtoAlloc &_Proto, size_t _Size) {
        using _Alloc = _Rebind<_ProtoAlloc>;
        auto _Al = static_cast<_Alloc>(_Proto);

        if constexpr (std::default_initializable<_Alloc> && std::allocator_traits<_Alloc>::is_always_equal::value) {
            // don't store stateless allocator
            // 不存储无状态分配器
            const _Dealloc_fn _Dealloc = [](void *const _Ptr,
                                            const size_t _Size) {
                _Alloc _Al{};
                const size_t _Count =
                    (_Size + sizeof(_Dealloc_fn) + sizeof(_Aligned_block) - 1)
                    / sizeof(_Aligned_block);
                _Al.deallocate(static_cast<_Aligned_block *>(_Ptr), _Count);
            };

            const size_t _Count =
                (_Size + sizeof(_Dealloc_fn) + sizeof(_Aligned_block) - 1)
                / sizeof(_Aligned_block);
            void *const _Ptr = _Al.allocate(_Count);
            ::memcpy(
                static_cast<char *>(_Ptr) + _Size, &_Dealloc, sizeof(_Dealloc)
            );
            return _Ptr;
        } else {
            // store stateful allocator
            // 存储有状态分配器
            static constexpr size_t _Align =
                (::std::max)(alignof(_Alloc), sizeof(_Aligned_block));

            const _Dealloc_fn _Dealloc = [](void *const _Ptr, size_t _Size) {
                _Size += sizeof(_Dealloc_fn);
                const auto _Al_address = (reinterpret_cast<uintptr_t>(_Ptr)
                                          + _Size + alignof(_Alloc) - 1)
                                         & ~(alignof(_Alloc) - 1);
                auto &_Stored_al =
                    *reinterpret_cast<const _Alloc *>(_Al_address);
                _Alloc _Al{::std::move(_Stored_al)};
                _Stored_al.~_Alloc();

                const size_t _Count =
                    (_Size + sizeof(_Al) + _Align - 1) / sizeof(_Aligned_block);
                _Al.deallocate(static_cast<_Aligned_block *>(_Ptr), _Count);
            };

            const size_t _Count =
                (_Size + sizeof(_Dealloc_fn) + sizeof(_Al) + _Align - 1)
                / sizeof(_Aligned_block);
            void *const _Ptr = _Al.allocate(_Count);
            ::memcpy(
                static_cast<char *>(_Ptr) + _Size, &_Dealloc, sizeof(_Dealloc)
            );
            _Size += sizeof(_Dealloc_fn);
            const auto _Al_address = (reinterpret_cast<uintptr_t>(_Ptr) + _Size
                                      + alignof(_Alloc) - 1)
                                     & ~(alignof(_Alloc) - 1);
            ::new (reinterpret_cast<void *>(_Al_address))
                _Alloc{::std::move(_Al)};
            return _Ptr;
        }
    }

  public:
    static void *operator new(const size_t _Size) { // default: new/delete
        void *const _Ptr = ::operator new[](_Size + sizeof(_Dealloc_fn));
        const _Dealloc_fn _Dealloc = [](void *const _Ptr, const size_t _Size) {
#if defined(__clang__)
            (void)_Size;
            ::operator delete[](_Ptr);
#else
            ::operator delete[](_Ptr, _Size + sizeof(_Dealloc_fn));
#endif
        };
        ::memcpy(
            static_cast<char *>(_Ptr) + _Size, &_Dealloc, sizeof(_Dealloc_fn)
        );
        return _Ptr;
    }

    template<class _Alloc, class... _Args>
    static void *operator new(
        const size_t _Size,
        std::allocator_arg_t,
        const _Alloc &_Al,
        const _Args &...
    ) {
        static_assert(
            _Has_real_pointers<_Alloc>,
            "coroutine allocators must use true pointers"
        );
        return _Allocate(_Al, _Size);
    }

    template<class _This, class _Alloc, class... _Args>
    static void *operator new(
        const size_t _Size,
        const _This &,
        std::allocator_arg_t,
        const _Alloc &_Al,
        const _Args &...
    ) {
        static_assert(
            _Has_real_pointers<_Alloc>,
            "coroutine allocators must use true pointers"
        );
        return _Allocate(_Al, _Size);
    }

    static void operator delete(void *const _Ptr, const size_t _Size) noexcept {
        _Dealloc_fn _Dealloc;
        ::memcpy(
            &_Dealloc, static_cast<const char *>(_Ptr) + _Size,
            sizeof(_Dealloc_fn)
        );
        _Dealloc(_Ptr, _Size);
    }
};

namespace ranges {
    /*
     * elements_of：generator 的嵌套 yield（yield 另一个 range 的所有元素）。
     *
     * 使用方式：
     *   generator<int> inner() { co_yield 1; co_yield 2; }
     *   generator<int> outer() {
     *       co_yield elements_of(inner());
     *       co_yield elements_of(std::vector{3, 4});
     *   }
     *   // 结果：1, 2, 3, 4
     *
     * 这类似于 Python 的 yield from 语法。
     * _Rng 可以是 generator、容器或任何 input_range。
     */
    template<std::ranges::range _Rng, class _Alloc = std::allocator<std::byte>>
    struct elements_of {
        NO_UNIQUE_ADDRESS _Rng range;
        NO_UNIQUE_ADDRESS _Alloc allocator{};
    };

    template<class _Rng, class _Alloc = std::allocator<std::byte>>
    elements_of(_Rng &&, _Alloc = {}) -> elements_of<_Rng &&, _Alloc>;
} // namespace ranges

/*
 * generator 主模板的前置声明。
 * 三个模板参数：
 * - _Rty：引用类型（决定迭代器解引用的返回类型）
 * - _Vty：值类型（决定 value_type，默认为 void 时使用 remove_cvref<_Rty>）
 * - _Alloc：协程帧分配器（默认为 void，使用全局 operator new/delete）
 */
template<class _Rty, class _Vty = void, class _Alloc = void>
class generator;

template<class _Rty, class _Vty>
using _Gen_value_t =
    std::conditional_t<std::is_void_v<_Vty>, std::remove_cvref_t<_Rty>, _Vty>;
template<class _Rty, class _Vty>
using _Gen_reference_t =
    std::conditional_t<std::is_void_v<_Vty>, _Rty &&, _Rty>;
template<class _Ref>
using _Gen_yield_t =
    std::conditional_t<std::is_reference_v<_Ref>, _Ref, const _Ref &>;

/*
 * _Gen_promise_base：generator 协程的 promise 基础类。
 *
 * 这是 generator 的核心状态管理类，处理：
 * - yield_value：将产生的值暴露给迭代器
 * - 嵌套 generator 的展开（elements_of 支持）
 * - 协程的初始/最终挂起逻辑
 * - 异常传播
 *
 * 嵌套 generator（yield_value for elements_of<generator>）：
 * 通过 _Nested_awaitable 实现递归展开。当 generator A 产生
 * elements_of(generator B) 时，A 挂起自己，B 开始执行产生值，
 * 迭代器透明地遍历 B 的所有值后将控制权返回给 A。
 *
 * _Final_awaiter 的设计：
 * 当协程执行完所有 yield 语句并正常结束时，final_suspend 被调用。
 * 它检查 _Info 判断当前是否在嵌套展开中：
 * - 如果是：恢复父协程（让外层 generator 继续 yield 自己的值）
 * - 如果不是：返回 noop_coroutine（表示迭代结束）
 */
template<class _Yielded>
class _Gen_promise_base {
  public:
    static_assert(std::is_reference_v<_Yielded>);

    /* [[nodiscard]] */ std::suspend_always initial_suspend() noexcept {
        return {};
    }

    [[nodiscard]]
    auto final_suspend() noexcept {
        return _Final_awaiter{};
    }

    [[nodiscard]]
    std::suspend_always yield_value(_Yielded _Val) noexcept {
        _Ptr = ::std::addressof(_Val);
        return {};
    }

    // clang-format off
    [[nodiscard]]
    auto yield_value(const std::remove_reference_t<_Yielded>& _Val)
        noexcept(std::is_nothrow_constructible_v<std::remove_cvref_t<_Yielded>, const std::remove_reference_t<_Yielded>&>)
        requires (std::is_rvalue_reference_v<_Yielded> &&
            std::constructible_from<std::remove_cvref_t<_Yielded>, const std::remove_reference_t<_Yielded>&>) {
        return _Element_awaiter{_Val};
    }

    // clang-format on

    // clang-format off
    template <class _Rty, class _Vty, class _Alloc, class _Unused>
        requires std::same_as<_Gen_yield_t<_Gen_reference_t<_Rty, _Vty>>, _Yielded>
    [[nodiscard]]
    auto yield_value(
        ::coronet::ranges::elements_of<generator<_Rty, _Vty, _Alloc>&&, _Unused> _Elem) noexcept {
        return _Nested_awaitable<_Rty, _Vty, _Alloc>{std::move(_Elem.range)};
    }

    // clang-format on

    // clang-format off
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
    template <::std::ranges::input_range _Rng, class _Alloc>
        requires std::convertible_to<::std::ranges::range_reference_t<_Rng>, _Yielded>
    [[nodiscard]]
    auto yield_value(::coronet::ranges::elements_of<_Rng, _Alloc> _Elem) noexcept {
        // clang-format on
        using _Vty = ::std::ranges::range_value_t<_Rng>;
        return _Nested_awaitable<_Yielded, _Vty, _Alloc>{
            [](std::allocator_arg_t, _Alloc,
               ::std::ranges::iterator_t<_Rng> _It,
               const ::std::ranges::sentinel_t<_Rng> _Se
            ) -> generator<_Yielded, _Vty, _Alloc> {
                for (; _It != _Se; ++_It) {
                    co_yield static_cast<_Yielded>(*_It);
                }
            }(std::allocator_arg, _Elem.allocator,
              ::std::ranges::begin(_Elem.range),
              ::std::ranges::end(_Elem.range))
        };
    }

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

    void await_transform() = delete;

    void return_void() noexcept {}

    void unhandled_exception() {
        if (_Info) {
            _Info->_Except = ::std::current_exception();
        } else {
            throw;
        }
    }

  private:
    /*
     * _Element_awaiter：用于支持 yield_value 的值传递重载。
     * 当 _Yielded 是右值引用时，值需要被移动/复制到 awaiter 中。
     * await_suspend 时将地址存入 _Ptr。
     */
    struct _Element_awaiter {
        std::remove_cvref_t<_Yielded> _Val;

        [[nodiscard]]
        constexpr bool await_ready() const noexcept {
            return false;
        }

        template<class _Promise>
        constexpr void await_suspend(std::coroutine_handle<_Promise> _Handle
        ) noexcept {
#ifdef __cpp_lib_is_pointer_interconvertible
            static_assert(std::is_pointer_interconvertible_base_of_v<
                          _Gen_promise_base, _Promise>);
#endif // __cpp_lib_is_pointer_interconvertible

            _Gen_promise_base &_Current = _Handle.promise();
            _Current._Ptr = ::std::addressof(_Val);
        }

        constexpr void await_resume() const noexcept {}
    };

    /*
     * _Nest_info：嵌套 generator 展开时的上下文信息。
     * - _Except：嵌套 generator 抛出的异常
     * - _Parent：外层 generator 的协程句柄
     * - _Root：最外层 generator 的协程句柄
     *   多个嵌套层次共用一个 _Root，用于 _Top 指针的快速访问
     */
    struct _Nest_info {
        std::exception_ptr _Except;
        std::coroutine_handle<_Gen_promise_base> _Parent;
        std::coroutine_handle<_Gen_promise_base> _Root;
    };

    /*
     * _Final_awaiter：协程最终的挂起等待器。
     *
     * await_suspend 逻辑：
     * 1. 如果 _Info 为空（没有嵌套），返回 noop_coroutine —— 迭代结束
     * 2. 如果有嵌套，获取父协程句柄 _Parent，恢复父协程以继续外层迭代
     * 3. 在恢复前更新最外层 generator 的 _Top 为当前嵌套的父节点，
     *    以确保 _Top 指针链的正确性
     */
    struct _Final_awaiter {
        [[nodiscard]]
        bool await_ready() noexcept {
            return false;
        }

        template<class _Promise>
        [[nodiscard]]
        std::coroutine_handle<>
        await_suspend(std::coroutine_handle<_Promise> _Handle) noexcept {
#ifdef __cpp_lib_is_pointer_interconvertible
            static_assert(std::is_pointer_interconvertible_base_of_v<
                          _Gen_promise_base, _Promise>);
#endif // __cpp_lib_is_pointer_interconvertible

            _Gen_promise_base &_Current = _Handle.promise();
            if (!_Current._Info) {
                return ::std::noop_coroutine();
            }

            std::coroutine_handle<_Gen_promise_base> _Cont =
                _Current._Info->_Parent;
            _Current._Info->_Root.promise()._Top = _Cont;
            _Current._Info = nullptr;
            return _Cont;
        }

        void await_resume() noexcept {}
    };

    /*
     * _Nested_awaitable：嵌套 generator 的等待器。
     *
     * 当 generator A 执行 co_yield elements_of(gen_B) 时：
     * 1. yield_value 创建一个 _Nested_awaitable，持有 gen_B 的拷贝
     * 2. 协程 A 的 await_suspend 保存 A 的上下文到 _Nested 中
     * 3. 返回 gen_B 的协程句柄，执行转移到 gen_B
     * 4. 迭代器通过 gen_B 产生值，直到 gen_B 结束
     * 5. gen_B 的 final_suspend 恢复 A
     * 6. A 的 await_resume 检查 gen_B 是否有异常
     *
     * _Top 指针链：
     * 最外层 generator 的 promise 持有 _Top，指向当前正在产生值的
     * generator 的 promise。迭代器通过 _Top.promise()._Ptr 获取当前值。
     * 这避免了在嵌套展开时多次解引用句柄链。
     */
    template<class _Rty, class _Vty, class _Alloc>
    struct _Nested_awaitable {
        static_assert(std::same_as<
                      _Gen_yield_t<_Gen_reference_t<_Rty, _Vty>>,
                      _Yielded>);

        _Nest_info _Nested;
        generator<_Rty, _Vty, _Alloc> _Gen;

        explicit _Nested_awaitable(generator<_Rty, _Vty, _Alloc> &&_Gen_
        ) noexcept
            : _Gen(::std::move(_Gen_)) {}

        [[nodiscard]]
        bool await_ready() noexcept {
            return !_Gen._Coro;
        }

        template<class _Promise>
        [[nodiscard]]
        std::coroutine_handle<_Gen_promise_base>
        await_suspend(std::coroutine_handle<_Promise> _Current) noexcept {
#ifdef __cpp_lib_is_pointer_interconvertible
            static_assert(std::is_pointer_interconvertible_base_of_v<
                          _Gen_promise_base, _Promise>);
#endif // __cpp_lib_is_pointer_interconvertible
            auto _Target =
                std::coroutine_handle<_Gen_promise_base>::from_address(
                    _Gen._Coro.address()
                );
            _Nested._Parent =
                std::coroutine_handle<_Gen_promise_base>::from_address(
                    _Current.address()
                );
            _Gen_promise_base &_Parent_promise = _Nested._Parent.promise();
            if (_Parent_promise._Info) {
                _Nested._Root = _Parent_promise._Info->_Root;
            } else {
                _Nested._Root = _Nested._Parent;
            }
            _Nested._Root.promise()._Top = _Target;
            _Target.promise()._Info = ::std::addressof(_Nested);
            return _Target;
        }

        void await_resume() {
            if (_Nested._Except) [[unlikely]] {
                ::std::rethrow_exception(::std::move(_Nested._Except));
            }
        }
    };

    template<class, class>
    friend class _Gen_iter;

    // _Top and _Info are mutually exclusive, and could potentially be merged.
    // _Top 和 _Info 互斥：有嵌套时用 _Info，无嵌套时用 _Top 指向自己
    std::coroutine_handle<_Gen_promise_base> _Top =
        std::coroutine_handle<_Gen_promise_base>::from_promise(*this);
    std::add_pointer_t<_Yielded> _Ptr = nullptr;
    _Nest_info *_Info = nullptr;
};

/*
 * _Gen_secret_tag：用于控制 generator 和 _Gen_iter 构造函数的访问权限。
 * 只有 friend 类和函数可以创建带有此 tag 的对象，防止用户错误地构造它们。
 */
struct _Gen_secret_tag {};

/*
 * _Gen_iter：generator 的迭代器。
 *
 * 迭代器使用 _Top 指针间接获取当前值，这是因为在嵌套 generator 的场景中，
 * 当前产生值的协程可能不是最外层的 generator。
 *
 * operator++ 会恢复 _Top 所指向的协程，推进 generator 到下一个 yield 点。
 * 当 _Top 协程 done() 时，迭代器等于 end()。
 */
template<class _Value, class _Ref>
class _Gen_iter {
  public:
    using value_type = _Value;
    using difference_type = ptrdiff_t;

    _Gen_iter(_Gen_iter &&_That) noexcept
        : _Coro{::std::exchange(_That._Coro, {})} {}

    _Gen_iter &operator=(_Gen_iter &&_That) noexcept {
        _Coro = ::std::exchange(_That._Coro, {});
        return *this;
    }

    [[nodiscard]]
    _Ref
    operator*() const noexcept {
        assert(!_Coro.done() && "Can't dereference generator end iterator");
        return static_cast<_Ref>(*_Coro.promise()._Top.promise()._Ptr);
    }

    _Gen_iter &operator++() {
        assert(!_Coro.done() && "Can't increment generator end iterator");
        _Coro.promise()._Top.resume();
        return *this;
    }

    void operator++(int) { ++*this; }

    [[nodiscard]]
    bool
    operator==(std::default_sentinel_t) const noexcept {
        return _Coro.done();
    }

  private:
    template<class, class, class>
    friend class generator;

    explicit _Gen_iter(
        _Gen_secret_tag,
        std::coroutine_handle<_Gen_promise_base<_Gen_yield_t<_Ref>>> _Coro_
    ) noexcept
        : _Coro{_Coro_} {}

    std::coroutine_handle<_Gen_promise_base<_Gen_yield_t<_Ref>>> _Coro;
};

/*
 * generator 类 —— 协程的返回类型，产生值序列。
 *
 * 使用方式：
 *   generator<int> range(int n) {
 *       for (int i = 0; i < n; ++i) co_yield i;
 *   }
 *   for (int x : range(5)) { /* 0, 1, 2, 3, 4 * / }
 *
 * promise_type 的继承链：
 *   promise_type : _Promise_allocator<_Alloc>, _Gen_promise_base<_Yielded>
 *   - _Promise_allocator 管理协程帧的内存分配
 *   - _Gen_promise_base 管理协程生命周期和值产生逻辑
 *
 * 使用 __declspec(empty_bases)（MSVC）确保两个基类中空的类不占用额外空间。
 * std::is_pointer_interconvertible_base_of 检查确保两个基类可以安全地
 * 在指针之间转换（这是静态转换而非 dynamic_cast 的前提）。
 *
 * generator 是 move-only 类型（复制构造函数被删除），
 * 因为协程句柄是独占资源。
 * 移动后将源对象置空（通过 std::exchange），防止 double destroy。
 */
template<class _Rty, class _Vty, class _Alloc>
class generator
    : public std::ranges::view_interface<generator<_Rty, _Vty, _Alloc>> {
  private:
    using _Value = _Gen_value_t<_Rty, _Vty>;
    static_assert(
        std::same_as<std::remove_cvref_t<_Value>, _Value>
            && std::is_object_v<_Value>,
        "generator's value type must be a cv-unqualified object type"
    );

    // clang-format off
    using _Ref = _Gen_reference_t<_Rty, _Vty>;
    static_assert(std::is_reference_v<_Ref>
        || (std::is_object_v<_Ref> && std::same_as<std::remove_cv_t<_Ref>, _Ref> && std::copy_constructible<_Ref>),
        "generator's second argument must be a reference type or a cv-unqualified "
        "copy-constructible object type");

    using _RRef = std::conditional_t<std::is_lvalue_reference_v<_Ref>, std::remove_reference_t<_Ref>&&, _Ref>;

    static_assert(std::common_reference_with<_Ref&&, _Value&> && std::common_reference_with<_Ref&&, _RRef&&>
        && std::common_reference_with<_RRef&&, const _Value&>,
        "an iterator with the selected value and reference types cannot model indirectly_readable");
    // clang-format on

    static_assert(
        _Has_real_pointers<_Alloc>,
        "generator allocators must use true pointers"
    );

    friend _Gen_promise_base<_Gen_yield_t<_Ref>>;

  public:
    struct EMPTY_BASES promise_type
        : _Promise_allocator<_Alloc>
        , _Gen_promise_base<_Gen_yield_t<_Ref>> {
        [[nodiscard]]
        generator get_return_object() noexcept {
            return generator{
                _Gen_secret_tag{},
                std::coroutine_handle<promise_type>::from_promise(*this)
            };
        }
    };

    static_assert(std::is_standard_layout_v<promise_type>);
#ifdef __cpp_lib_is_pointer_interconvertible
    static_assert(std::is_pointer_interconvertible_base_of_v<
                  _Gen_promise_base<_Gen_yield_t<_Ref>>,
                  promise_type>);
#endif // __cpp_lib_is_pointer_interconvertible

    generator(generator &&_That) noexcept
        : _Coro(::std::exchange(_That._Coro, {})) {}

    ~generator() {
        if (_Coro) {
            _Coro.destroy();
        }
    }

    generator &operator=(generator _That) noexcept {
        ::std::swap(_Coro, _That._Coro);
        return *this;
    }

    [[nodiscard]]
    _Gen_iter<_Value, _Ref> begin() {
        // Pre: _Coro is suspended at its initial suspend point
        // 前提：_Coro 在初始挂起点已挂起
        assert(_Coro && "Can't call begin on moved-from generator");
        _Coro.resume();
        return _Gen_iter<_Value, _Ref>{
            _Gen_secret_tag{},
            std::coroutine_handle<_Gen_promise_base<_Gen_yield_t<_Ref>>>::
                from_address(_Coro.address())
        };
    }

    [[nodiscard]]
    std::default_sentinel_t end() const noexcept {
        return std::default_sentinel;
    }

  private:
    std::coroutine_handle<promise_type> _Coro = nullptr;

    explicit generator(
        _Gen_secret_tag, std::coroutine_handle<promise_type> _Coro_
    ) noexcept
        : _Coro(_Coro_) {}
};
}

// NOLINTEND
#endif
