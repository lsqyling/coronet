/// coroutine composition: when_all / when_any / when_some
#pragma once

#include <coronet/io_context.hpp>
#include <coronet/task.hpp>
#include <coronet/shared_task.hpp>
#include <coronet/async_io.hpp>
#include <coronet/detail/uninitialized_buffer.hpp>

#include <atomic>
#include <coroutine>
#include <memory>
#include <mutex>
#include <tuple>
#include <variant>
#include <vector>

namespace coronet {

template<typename... Ts> struct overload : Ts... { using Ts::operator()...; };
template<typename... Ts> overload(Ts...) -> overload<Ts...>;

namespace detail {

// ============================================================
// when_all: shared state with atomic countdown
// ============================================================
template<typename ResultTuple>
struct when_all_state {
    std::atomic<uint32_t> remaining;
    std::coroutine_handle<> awaiting{nullptr};

    // Storage: one uninitialized_buffer per non-void result
    template<typename> struct to_buf { using type = void; };
    template<typename... Ts> struct to_buf<std::tuple<Ts...>> {
        using type = std::tuple<uninitialized_buffer<Ts>...>;
    };
    typename to_buf<ResultTuple>::type storage;

    explicit when_all_state(uint32_t n) : remaining(n) {}

    void count_down() noexcept {
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
            awaiting.resume();
    }

    ResultTuple take() noexcept {
        return std::apply([](auto&... buf) {
            return ResultTuple{std::move(buf.ref())...};
        }, storage);
    }
};

// Helper: execute one task, store result at position ResultIdx.
// Takes any co_awaitable type (task<T>, shared_task<T>, etc.)
template<size_t ResultIdx, typename State, typename T, typename Node>
task<void> all_run_one(Node node, std::shared_ptr<State> st) {
    if constexpr (!std::is_void_v<T>) {
        auto& buf = std::get<ResultIdx>(st->storage);
        buf.construct(co_await std::move(node));
    } else {
        co_await std::move(node);
    }
    st->count_down();
}

// ============================================================
// when_any / when_some
// ============================================================
template<typename Variant>
struct when_any_state {
    std::atomic<uint32_t> completed{0};
    uint32_t needed;
    std::coroutine_handle<> awaiting{nullptr};
    std::mutex mtx;

    uint32_t winner_idx{0};
    Variant winner_value{};
    std::vector<std::pair<uint32_t, Variant>> results;

    explicit when_any_state(uint32_t n) : needed(n) {}

    bool on_complete(uint32_t idx, Variant val) {
        std::lock_guard lock(mtx);
        if (resumed) return false; // already done — ignore late completions
        if (completed == 0) { winner_idx = idx; winner_value = val; }
        results.emplace_back(idx, std::move(val));
        uint32_t prev = completed.fetch_add(1);
        bool done = (prev + 1 >= needed);
        if (done) resumed = true; // prevent double-resume (set under lock)
        return done;
    }
    bool resumed{false};
};

template<size_t TaskIdx, size_t VarIdx, typename State, typename T, typename Variant, typename Node>
task<void> any_run_one(Node t, std::shared_ptr<State> st) {
    if constexpr (std::is_void_v<T>) {
        co_await std::move(t);
        if (st->on_complete(TaskIdx, Variant{std::monostate{}}))
            st->awaiting.resume();
    } else {
        Variant v;
        v.template emplace<VarIdx + 1>(co_await std::move(t));
        if (st->on_complete(TaskIdx, std::move(v)))
            st->awaiting.resume();
    }
}

// ============================================================
// Type helpers: filter void, build variant/tuple types
// ============================================================
template<typename...> struct void_filter;
template<> struct void_filter<> { using tuple = std::tuple<>; };
template<typename T, typename... Rest> struct void_filter<T, Rest...> {
    using tail = typename void_filter<Rest...>::tuple;
    using tuple = std::conditional_t<std::is_void_v<T>, tail,
        decltype(std::tuple_cat(std::declval<std::tuple<T>>(), std::declval<tail>()))>;
};

// Build variant: std::monostate + non-void types (no duplicates)
template<typename... Ts> struct build_variant;
template<typename... Ts> struct build_variant {
    using filtered = typename void_filter<Ts...>::tuple;
    template<typename> struct prepend_monostate;
    template<typename... Us> struct prepend_monostate<std::tuple<Us...>> {
        using type = std::variant<std::monostate, Us...>;
    };
    using type = typename prepend_monostate<filtered>::type;
};

// Compute the non-void result index for a given task position
template<size_t Pos, typename... Types>
constexpr size_t non_void_idx = []() constexpr {
    size_t r = 0;
    size_t i = 0;
    auto scan = [&]<typename U>() {
        if (i == Pos) return;
        if (!std::is_void_v<U>) ++r;
        ++i;
    };
    (scan.template operator()<Types>(), ...);
    return r;
}();

} // namespace detail

// ============================================================
// all(task0, task1, ...) → tuple<non-void results>
// ============================================================
template<typename... TaskTypes>
task<typename detail::void_filter<typename TaskTypes::value_type...>::tuple>
all(TaskTypes... tasks) {
    using result_t = typename detail::void_filter<typename TaskTypes::value_type...>::tuple;
    constexpr uint32_t N = static_cast<uint32_t>(sizeof...(TaskTypes));

    if constexpr (std::is_same_v<result_t, std::tuple<>>) {
        // All void: just countdown
        struct void_state { std::atomic<uint32_t> rem; std::coroutine_handle<> h{nullptr}; };
        auto st = std::make_shared<void_state>(N);
        auto run_void = [](auto t, std::shared_ptr<void_state> s) -> task<void> {
            co_await std::move(t);
            if (s->rem.fetch_sub(1) == 1) s->h.resume();
        };
        auto spawn = [&]<size_t... Idx>(std::index_sequence<Idx...>) {
            (co_spawn(run_void(static_cast<TaskTypes&&>(tasks), st)), ...);
        };
        spawn(std::index_sequence_for<TaskTypes...>{});
        struct awaiter {
            std::shared_ptr<void_state> s;
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h) noexcept { s->h = h; }
            void await_resume() const noexcept {}
        };
        co_await awaiter{st};
        co_return;
    } else {
        using state_t = detail::when_all_state<result_t>;
        auto st = std::make_shared<state_t>(N);

        // Spawn each task via fold over index sequence
        [&]<size_t... Idx>(std::index_sequence<Idx...>) {
            (([]<size_t I>(std::shared_ptr<state_t> s, auto& t) {
                using val_t = typename std::tuple_element_t<
                    I, std::tuple<TaskTypes...>>::value_type;
                constexpr size_t ridx = detail::non_void_idx<
                    I, typename TaskTypes::value_type...>;
                co_spawn(detail::all_run_one<ridx, state_t, val_t>(
                    std::move(t), s));
            }).template operator()<Idx>(st, tasks), ...);
        }(std::index_sequence_for<TaskTypes...>{});

        struct awaiter {
            std::shared_ptr<state_t> s;
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h) noexcept { s->awaiting = h; }
            result_t await_resume() noexcept { return s->take(); }
        };
        co_return co_await awaiter{st};
    }
}

// ============================================================
// any(task0, task1, ...) → pair<idx, variant>
// ============================================================
template<typename... TaskTypes>
    requires (sizeof...(TaskTypes) >= 2)
task<std::pair<uint32_t, typename detail::build_variant<typename TaskTypes::value_type...>::type>>
any(TaskTypes... tasks) {
    using variant_t = typename detail::build_variant<typename TaskTypes::value_type...>::type;
    using core_t = detail::when_any_state<variant_t>;
    auto st = std::make_shared<core_t>(1);

    [&]<size_t... Idx>(std::index_sequence<Idx...>) {
        ([]<size_t I>(auto&& tsk, std::shared_ptr<core_t> s) {
            constexpr size_t vi = detail::non_void_idx<I, typename TaskTypes::value_type...>;
            co_spawn(detail::any_run_one<I, vi, core_t, typename TaskTypes::value_type,
                      variant_t>(std::forward<decltype(tsk)>(tsk), s));
        }.template operator()<Idx>(static_cast<TaskTypes&&>(tasks), st), ...);
    }(std::index_sequence_for<TaskTypes...>{});

    struct awaiter {
        std::shared_ptr<core_t> s;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept { s->awaiting = h; }
        std::pair<uint32_t, variant_t> await_resume() noexcept {
            s->resumed = true;
            return {s->winner_idx, std::move(s->winner_value)};
        }
    };
    co_return co_await awaiter{st};
}

// ============================================================
// some(n, task0, task1, ...) → vector<pair<idx, variant>>
// ============================================================
template<typename... TaskTypes>
    requires (sizeof...(TaskTypes) >= 2)
task<std::vector<std::pair<uint32_t, typename detail::build_variant<typename TaskTypes::value_type...>::type>>>
some(uint32_t need, TaskTypes... tasks) {
    using variant_t = typename detail::build_variant<typename TaskTypes::value_type...>::type;
    using core_t = detail::when_any_state<variant_t>;
    auto st = std::make_shared<core_t>(need);

    [&]<size_t... Idx>(std::index_sequence<Idx...>) {
        ([]<size_t I>(auto&& tsk, std::shared_ptr<core_t> s) {
            constexpr size_t vi = detail::non_void_idx<I, typename TaskTypes::value_type...>;
            co_spawn(detail::any_run_one<I, vi, core_t, typename TaskTypes::value_type,
                      variant_t>(std::forward<decltype(tsk)>(tsk), s));
        }.template operator()<Idx>(static_cast<TaskTypes&&>(tasks), st), ...);
    }(std::index_sequence_for<TaskTypes...>{});

    struct awaiter {
        std::shared_ptr<core_t> s;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept { s->awaiting = h; }
        std::vector<std::pair<uint32_t, variant_t>> await_resume() noexcept {
            s->resumed = true;
            return std::move(s->results);
        }
    };
    co_return co_await awaiter{st};
}

} // namespace coronet
