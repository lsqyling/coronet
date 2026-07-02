/// Generator test for coronet::generator<R, V, Alloc>
/// Ported from co_context/test/generator_test.cpp
///
/// 本文件是对 coronet::generator 的手动测试套件，不使用任何测试框架，
/// 直接通过 assert 和 printf 进行验证。
///
/// generator 基于 C++20 协程的 co_yield 关键字，提供一个惰性求值的序列生成器。
/// 它完全符合 std::generator 的语义，是 P2502R2 提案的参考实现。
///
/// 与 generator_gtest.cpp 相比，本测试更全面，覆盖了以下高级特性：
///   - 嵌套序列生成（通过 elements_of 将子生成器扁平化展开）
///   - 值类型 / 左值引用 / 右值引用 / 常量引用等多种返回语义
///   - 仅移动类型（std::unique_ptr）的支持
///   - 自定义分配器支持（有状态分配器和无状态分配器）
///   - value_type 转换（如将 std::string_view 转换为 std::string）
///   - 进阶用法：zip 组合子（将多个生成器按元素位置打包为元组序列）
///   - 成员函数作为生成器协程

#include <coronet/generator.hpp>

#include <array>
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace {

// =========================================================
// Simple non-nested serial generator
// =========================================================

// 斐波那契数列生成器，产生产 max 个元素
// 使用 std::exchange 实现 a 和 b 的原子级交换更新，这是 C++17 的风格
// co_yield 逐个产出当前斐波那契数，每次 yield 后协程挂起
coronet::generator<uint64_t> fib(int max) {
    auto a = 0, b = 1;
    for (auto n = 0; n < max; n++) {
        co_yield std::exchange(a, std::exchange(b, a + b));
    }
}

// 简单的递增序列生成器，从 i 到 j（不包含 j）
// 生成闭区间 [i, j) 上的整数序列
coronet::generator<int> other_generator(int i, int j) {
    while (i != j) {
        co_yield i++;
    }
}

// =========================================================
// Nested sequences via elements_of
// =========================================================

// 嵌套序列生成示例：使用 elements_of 将多个序列扁平化为单一序列
// elements_of 是 P2502R2 中引入的关键特性，允许生成器嵌套子生成器
// 调用方看到的是一个扁平化的序列，包含：
//   1. std::array 的 5 个元素
//   2. fib(10) 生成的 10 个斐波那契数
//   3. other_generator(5, 8) 生成的 3 个整数（5, 6, 7）
// 总计 18 个元素
coronet::generator<uint64_t, uint64_t> nested_sequences_example() {
    std::printf("yielding elements_of std::array\n");
    co_yield coronet::ranges::elements_of{
        std::array<const uint64_t, 5>{2, 4, 6, 8, 10}
    };
    std::printf("yielding elements_of nested coronet::generator\n");
    co_yield coronet::ranges::elements_of{fib(10)};
    std::printf("yielding elements_of other kind of generator\n");
    co_yield coronet::ranges::elements_of{other_generator(5, 8)};
}

// =========================================================
// Reference/value type examples
// =========================================================

// 用于跟踪构造/拷贝/移动/析构的辅助类型
// 每个构造函数和析构函数都会打印日志，方便观察对象的生命周期
// 通过 id 字段追踪对象的标识，移动操作会将源对象的 id 置为 -1
struct X {
    int id;
    X(int id) : id(id) { std::printf("X::X(%i)\n", id); }
    X(const X& x) : id(x.id) { std::printf("X::X(copy %i)\n", id); }
    X(X&& x) : id(std::exchange(x.id, -1)) {
        std::printf("X::X(move %i)\n", id);
    }
    ~X() { std::printf("X::~X(%i)\n", id); }
};

// 值类型的生成器 —— 生成 X 类型对象的右值
// 演示不同的 co_yield 语法：
//   1. co_yield X{1} ：yield 临时对象（纯右值）
//   2. co_yield x    ：yield 左值，会触发拷贝构造
//   3. co_yield std::move(x) ：显式移动语义
// 通过 assert 验证 yield 后原始对象的状态
coronet::generator<X> always_ref_example() {
    co_yield X{1};
    {
        X x{2};
        co_yield x;
        assert(x.id == 2);
    }
    {
        const X x{3};
        co_yield x;
        assert(x.id == 3);
    }
    {
        X x{4};
        co_yield std::move(x);
    }
}

// 右值引用生成器 —— 生成 X&& 类型
// 演示不同表达式作为右值引用时的行为：
//   - 临时对象（纯右值）绑定到右值引用
//   - 左值绑定到右值引用时，调用方看到的是左值的引用（而非拷贝）
//   - std::move 显式转换为右值引用
// 注意：X&& 类型的生成器允许调用方通过移动获取元素的所有权
coronet::generator<X&&> xvalue_example() {
    co_yield X{1};
    X x{2};
    co_yield x;
    assert(x.id == 2);
    co_yield std::move(x);
}

// 常量左值引用生成器 —— 生成 const X& 类型
// 调用方只能读取元素，不能修改
// 临时对象可以绑定到 const 引用（生命周期延长到引用作用域结束）
coronet::generator<const X&> const_lvalue_example() {
    co_yield X{1};
    const X x{2};
    co_yield x;
    co_yield std::move(x);
}

// 左值引用生成器 —— 生成 X& 类型
// 调用方可以获取和修改生成器内部对象的引用
// 这是最灵活的引用形式，但也需要调用方注意引用有效性
coronet::generator<X&> lvalue_example() {
    X x{2};
    co_yield x;
}

// =========================================================
// Move-only types
// =========================================================

// 仅移动类型的生成器，生成 std::unique_ptr<int> 的右值引用
// 验证 generator 对不可复制类型的支持
// unique_ptr 的所有权通过 co_yield 从生成器转移到调用方
coronet::generator<std::unique_ptr<int>&&> unique_ints(const int high) {
    for (auto i = 0; i < high; ++i) {
        co_yield std::make_unique<int>(i);
    }
}

// =========================================================
// String view / value_type examples
// =========================================================

// 字符串视图生成器 —— 产生 std::string_view 元素
// string_view 是轻量级的字符串引用，不涉及内存分配和拷贝
coronet::generator<std::string_view> string_views() {
    co_yield "foo";
    co_yield "bar";
}

// 带 value_type 转换的生成器 —— 产生 std::string 但引用类型为 std::string_view
// 模板参数：
//   R = std::string_view （引用类型，迭代器解引用返回的类型）
//   V = std::string     （值类型，range_value_t 推导的类型）
// 这使得调用方可以用范围 for 循环获取 std::string，同时生成器内部以 string_view 产出
// 当调用方需要具体类型（如字符串拼接）时特别有用
// 另外演示了自定义分配器的支持（通过 std::allocator_arg 标签传递分配器）
template<typename Allocator>
coronet::generator<std::string_view, std::string>
strings(std::allocator_arg_t, Allocator) {
    co_yield {};
    co_yield "start";
    for (auto sv : string_views()) {
        co_yield std::string{sv} + '!';
    }
    co_yield "end";
}

// 辅助函数：将任意输入范围（input_range）转换为 std::vector
// 使用完美转发保留元素的引用语义
template<std::ranges::input_range R>
std::vector<std::ranges::range_value_t<R>> to_vector(R&& r) {
    std::vector<std::ranges::range_value_t<R>> v;
    for (auto&& x : r) {
        v.emplace_back(static_cast<decltype(x)&&>(x));
    }
    return v;
}

// 进阶特性：zip 组合子 —— 将多个范围同步压缩为一个元组序列
// 同时迭代多个范围，每次产生一个包含各范围当前元素的元组
// 迭代在任意一个范围的元素耗尽时停止（最短范围决定长度）
// 这是 C++23 范围适配器的一个协程实现示例
template<std::ranges::range... Rs, std::size_t... Indices>
coronet::generator<
    std::tuple<std::ranges::range_reference_t<Rs>...>,
    std::tuple<std::ranges::range_value_t<Rs>...>>
zip_impl(std::index_sequence<Indices...>, Rs... rs) {
    std::tuple<std::ranges::iterator_t<Rs>...> its{std::ranges::begin(rs)...};
    std::tuple<std::ranges::sentinel_t<Rs>...> itEnds{std::ranges::end(rs)...};
    while (((std::get<Indices>(its) != std::get<Indices>(itEnds)) && ...)) {
        co_yield {*std::get<Indices>(its)...};
        (++std::get<Indices>(its), ...);
    }
}

template<std::ranges::range... Rs>
auto zip(Rs&&... rs) {
    return zip_impl(
        std::index_sequence_for<Rs...>{},
        std::views::all(std::forward<Rs>(rs))...
    );
}

void value_type_example() {
    std::vector<std::string_view> s1 = to_vector(string_views());
    for (auto& s : s1) {
        std::printf("\"%.*s\"\n", (int)s.size(), s.data());
    }
    std::printf("\n");
    std::vector<std::string> s2 =
        to_vector(strings(std::allocator_arg, std::allocator<std::byte>{}));
    for (auto& s : s2) {
        std::printf("\"%s\"\n", s.c_str());
    }
}

// =========================================================
// Allocator support
// =========================================================

// 有状态分配器示例：区分不同实例的分配器
// 每个实例有一个唯一的 id，用于在日志中区分分配请求来自哪个分配器
// 这是 C++ 分配器模型中的高级用法，用于内存池或区域分配等场景
// 实现 Allocator 概念需要：value_type、allocate、deallocate、比较运算符
template<typename T>
struct stateful_allocator {
    using value_type = T;
    int id;
    explicit stateful_allocator(int id) noexcept : id(id) {}
    template<typename U>
    stateful_allocator(const stateful_allocator<U>& x) : id(x.id) {}
    T* allocate(std::size_t count) {
        std::printf("stateful_allocator(%i).allocate(%zu)\n", id, count);
        return std::allocator<T>().allocate(count);
    }
    void deallocate(T* ptr, std::size_t count) noexcept {
        std::printf("stateful_allocator(%i).deallocate(%zu)\n", id, count);
        std::allocator<T>().deallocate(ptr, count);
    }
    template<typename U>
    bool operator==(const stateful_allocator<U>& x) const {
        return this->id == x.id;
    }
};

// 无状态分配器示例：使用默认的 std::allocator<std::byte>
// 这是最简单的情况，分配器不携带额外状态
coronet::generator<int, void, std::allocator<std::byte>>
stateless_example() {
    co_yield 42;
}

// 显式传递无状态分配器（通过 std::allocator_arg 标签）
// 虽然传递了 std::allocator<float>，但由于分配器是无状态的，
// 任何 allocator<T> 实例都是等价的，实际效果与默认构造相同
coronet::generator<int, void, std::allocator<std::byte>>
stateless_example_2(std::allocator_arg_t, std::allocator<std::byte>) {
    co_yield 42;
}

// 有状态分配器示例：分配器类型作为模板参数传入
// 有状态分配器会存储其状态（如 id），并在分配/释放内存时使用
// 这对内存分析和性能优化很有价值
template<typename Allocator>
coronet::generator<int, void, Allocator>
stateful_alloc_example(std::allocator_arg_t, Allocator) {
    co_yield 42;
}

// 成员函数作为生成器协程
// 演示 generator 也可以作为类的成员函数返回类型
struct member_coro {
    coronet::generator<int> f() const { co_yield 42; }
};

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);

    // Test 1: Simple fibonacci generator
    //
    // 验证基础斐波那契生成器
    // fib(10) 应恰好产出 10 个元素
    // 通过 count 计数验证元素数量
    std::printf("=== fibonacci ===\n");
    int count = 0;
    for (uint64_t a : fib(10)) {
        std::printf("-> %" PRIu64 "\n", a);
        count++;
    }
    assert(count == 10);

    // Test 2: Nested sequences via elements_of
    //
    // 验证 elements_of 嵌套序列展开
    // 总元素数应为：5（array）+ 10（fib）+ 3（other_generator 5,6,7）= 18
    // 这是 P2502R2 中的核心新特性，验证生成器可以扁平化嵌套的子生成器
    std::printf("\nnested_sequences_example\n");
    count = 0;
    for (uint64_t a : nested_sequences_example()) {
        std::printf("-> %" PRIu64 "\n", a);
        count++;
    }
    assert(count == 18); // 5 (array) + 10 (fib) + 3 (other: 5,6,7)

    // Test 3: Reference/value type examples
    //
    // 验证各种引用/值类型的生成器行为
    // 通过日志观察不同引用类型下的构造/拷贝/移动/析构过程
    // 验证生成器正确保留了值语义和引用语义
    std::printf("\nby_value_example\n");
    for (auto&& x : always_ref_example()) {
        std::printf("-> %i\n", x.id);
    }
    std::printf("\nby_rvalue_ref_example\n");
    for (auto&& x : xvalue_example()) {
        std::printf("-> %i\n", x.id);
    }
    std::printf("\nby_const_ref_example\n");
    for (auto&& x : const_lvalue_example()) {
        std::printf("-> %i\n", x.id);
    }
    std::printf("\nby_lvalue_ref_example\n");
    for (auto&& x : lvalue_example()) {
        std::printf("-> %i\n", x.id);
    }

    // Test 4: value_type example
    //
    // 验证 value_type 转换功能
    // string_views() 产出 string_view，通过 to_vector 转换为向量
    // strings() 使用自定义的引用类型/值类型组合进行转换
    std::printf("\nvalue_type example\n");
    value_type_example();

    // Test 5: move-only types
    //
    // 验证仅移动类型的生成器
    // unique_ints(5) 应产出 5 个 unique_ptr<int>
    // 每个元素通过 unique_ptr 的所有权转移传递给循环体
    std::printf("\nmove_only example\n");
    count = 0;
    for (std::unique_ptr<int> ptr : unique_ints(5)) {
        std::printf("-> %i\n", *ptr);
        count++;
    }
    assert(count == 5);

    // Test 6: Allocator examples
    //
    // 验证分配器支持
    // 测试三种分配器场景：
    //   1. 无状态分配器（默认构造）
    //   2. 显式传递的无状态分配器
    //   3. 有状态分配器（携带 id 信息）
    // 分配器用于生成器内部协程状态的动态内存分配
    std::printf("\nstateless_alloc examples\n");
    stateless_example();
    stateless_example_2(std::allocator_arg, std::allocator<float>{});
    std::printf("\nstateful_alloc example\n");
    stateful_alloc_example(std::allocator_arg, stateful_allocator<double>{42});

    // Test 7: Member coroutine
    //
    // 验证成员函数作为生成器
    // 通过结构体成员函数返回生成器，解引用 begin() 迭代器获取值
    // 验证生成器在类成员上下文中的可用性
    [[maybe_unused]] member_coro m;
    assert(*m.f().begin() == 42);

    std::printf("\nAll generator tests passed!\n");
    return 0;
}
