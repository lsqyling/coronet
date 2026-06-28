/// Generator test for coronet::generator<R, V, Alloc>
/// Ported from co_context/test/generator_test.cpp

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

coronet::generator<uint64_t> fib(int max) {
    auto a = 0, b = 1;
    for (auto n = 0; n < max; n++) {
        co_yield std::exchange(a, std::exchange(b, a + b));
    }
}

coronet::generator<int> other_generator(int i, int j) {
    while (i != j) {
        co_yield i++;
    }
}

// =========================================================
// Nested sequences via elements_of
// =========================================================

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

struct X {
    int id;
    X(int id) : id(id) { std::printf("X::X(%i)\n", id); }
    X(const X& x) : id(x.id) { std::printf("X::X(copy %i)\n", id); }
    X(X&& x) : id(std::exchange(x.id, -1)) {
        std::printf("X::X(move %i)\n", id);
    }
    ~X() { std::printf("X::~X(%i)\n", id); }
};

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

coronet::generator<X&&> xvalue_example() {
    co_yield X{1};
    X x{2};
    co_yield x;
    assert(x.id == 2);
    co_yield std::move(x);
}

coronet::generator<const X&> const_lvalue_example() {
    co_yield X{1};
    const X x{2};
    co_yield x;
    co_yield std::move(x);
}

coronet::generator<X&> lvalue_example() {
    X x{2};
    co_yield x;
}

// =========================================================
// Move-only types
// =========================================================

coronet::generator<std::unique_ptr<int>&&> unique_ints(const int high) {
    for (auto i = 0; i < high; ++i) {
        co_yield std::make_unique<int>(i);
    }
}

// =========================================================
// String view / value_type examples
// =========================================================

coronet::generator<std::string_view> string_views() {
    co_yield "foo";
    co_yield "bar";
}

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

template<std::ranges::input_range R>
std::vector<std::ranges::range_value_t<R>> to_vector(R&& r) {
    std::vector<std::ranges::range_value_t<R>> v;
    for (auto&& x : r) {
        v.emplace_back(static_cast<decltype(x)&&>(x));
    }
    return v;
}

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

coronet::generator<int, void, std::allocator<std::byte>>
stateless_example() {
    co_yield 42;
}

coronet::generator<int, void, std::allocator<std::byte>>
stateless_example_2(std::allocator_arg_t, std::allocator<std::byte>) {
    co_yield 42;
}

template<typename Allocator>
coronet::generator<int, void, Allocator>
stateful_alloc_example(std::allocator_arg_t, Allocator) {
    co_yield 42;
}

struct member_coro {
    coronet::generator<int> f() const { co_yield 42; }
};

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);

    // Test 1: Simple fibonacci generator
    std::printf("=== fibonacci ===\n");
    int count = 0;
    for (uint64_t a : fib(10)) {
        std::printf("-> %" PRIu64 "\n", a);
        count++;
    }
    assert(count == 10);

    // Test 2: Nested sequences via elements_of
    std::printf("\nnested_sequences_example\n");
    count = 0;
    for (uint64_t a : nested_sequences_example()) {
        std::printf("-> %" PRIu64 "\n", a);
        count++;
    }
    assert(count == 18); // 5 (array) + 10 (fib) + 3 (other: 5,6,7)

    // Test 3: Reference/value type examples
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
    std::printf("\nvalue_type example\n");
    value_type_example();

    // Test 5: move-only types
    std::printf("\nmove_only example\n");
    count = 0;
    for (std::unique_ptr<int> ptr : unique_ints(5)) {
        std::printf("-> %i\n", *ptr);
        count++;
    }
    assert(count == 5);

    // Test 6: Allocator examples
    std::printf("\nstateless_alloc examples\n");
    stateless_example();
    stateless_example_2(std::allocator_arg, std::allocator<float>{});
    std::printf("\nstateful_alloc example\n");
    stateful_alloc_example(std::allocator_arg, stateful_allocator<double>{42});

    // Test 7: Member coroutine
    [[maybe_unused]] member_coro m;
    assert(*m.f().begin() == 42);

    std::printf("\nAll generator tests passed!\n");
    return 0;
}
