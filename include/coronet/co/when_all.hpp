/// coroutine composition: when_all / when_any / when_some
///
/// 协程组合器 —— 并发执行多个协程任务并组合结果。
///
/// ## 设计模式：共享状态 + 原子倒计数（shared state + atomic countdown）
///
/// 三个组合器都基于一个共享状态（shared_ptr 管理的对象），该状态包含：
///   - 原子计数器：跟踪未完成的任务数
///   - 挂起协程句柄：所有任务完成后恢复调用者
///   - 结果存储：每个任务的结果存储在独立的槽位中
///
/// ## 为什么用 co_spawn 启动子任务？
/// 所有子任务通过 co_spawn 独立启动，而非在同一个协程中顺序执行。
/// 这使得它们能够真正并发执行（由 io_context 的多个 worker 线程调度）。
/// 每个子任务完成后递减原子计数器，最后一个完成的子任务恢复调用者协程。
///
/// ## 为什么用 shared_ptr 管理状态？
/// 子任务和调用者协程之间是 1:N 的关系，调用者协程可能先于子任务完成
/// （在 await_resume 中返回结果），但子任务仍可能（在清理过程中）访问共享状态。
/// shared_ptr 保证了在所有引用消失之前状态不会被销毁。
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

/// 重载模式辅助类型 —— 用于 std::visit 的 auto&&... 重载。
template<typename... Ts> struct overload : Ts... { using Ts::operator()...; };
template<typename... Ts> overload(Ts...) -> overload<Ts...>;

namespace detail {

// ============================================================
// when_all: shared state with atomic countdown
// ============================================================
// when_all 的核心共享状态。
//
// 原理：初始化 remaining = N（任务总数），每个任务完成后递减。
// 当 remaining 从 1→0 时（fetch_sub 返回 1），说明自己是最后一个完成的，
// 此时恢复调用者协程。
//
// storage 是一个元组，每个非 void 类型的结果对应一个 uninitialized_buffer。
// 这避免了默认构造 T 的开销 —— buffer 直到结果就绪才构造。
//
// ResultTuple: all non-void result types as tuple
template<typename ResultTuple>
struct when_all_state {
    std::atomic<uint32_t> remaining;     // 剩余未完成任务数（原子倒计数）
    std::coroutine_handle<> awaiting{nullptr};  // 调用者协程句柄

    // 存储类型推导：为元组中每个非 void 类型生成 uninitialized_buffer
    template<typename> struct to_buf { using type = void; };
    template<typename... Ts> struct to_buf<std::tuple<Ts...>> {
        using type = std::tuple<uninitialized_buffer<Ts>...>;
    };
    typename to_buf<ResultTuple>::type storage;

    explicit when_all_state(uint32_t n) : remaining(n) {}

    /// 递减计数器，若到零则恢复调用者。
    ///
    /// Count down, resume caller if this was the last task.
    void count_down() noexcept {
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
            awaiting.resume();
    }

    /// 取出所有结果 —— 将每个 uninitialized_buffer 中的值移动出来。
    ///
    /// Take all results — move out values from each buffer.
    ResultTuple take() noexcept {
        return std::apply([](auto&... buf) {
            return ResultTuple{std::move(buf.ref())...};
        }, storage);
    }
};

// Helper: execute one task, store result at position ResultIdx.
// Takes any co_awaitable type (task<T>, shared_task<T>, etc.)
//
// 辅助函数：执行单个子任务，将结果存入共享状态的对应槽位。
// ResultIdx 是结果在整个元组中的位置（跳过了 void 类型）。
template<size_t ResultIdx, typename State, typename T, typename Node>
task<void> all_run_one(Node node, std::shared_ptr<State> st) {
    if constexpr (!std::is_void_v<T>) {
        // 非 void 结果：构造到对应的 storage 槽位
        auto& buf = std::get<ResultIdx>(st->storage);
        buf.construct(co_await std::move(node));
    } else {
        // void 结果：只执行，不存储
        co_await std::move(node);
    }
    st->count_down();
}

// ============================================================
// when_any / when_some
// ============================================================
// when_any/when_some 的共享状态。
//
// 与 when_all 不同，这里用 std::mutex 保护结果收集（而非原子操作），
// 因为需要存储多个结果并检查 resumed 标志。
//
// 关键设计：resumed 标志在 std::mutex 保护下设置，防止在临界条件
// （一个线程正在标记完成，另一个线程正在恢复）下的竞态条件。
template<typename Variant>
struct when_any_state {
    std::atomic<uint32_t> completed{0};  // 已完成的任务数
    uint32_t needed;                     // 需要完成的任务数
    std::coroutine_handle<> awaiting{nullptr};  // 调用者协程句柄
    std::mutex mtx;                      // 保护结果收集的互斥锁

    uint32_t winner_idx{0};              // 第一个完成的任务索引
    Variant winner_value{};              // 第一个完成的任务结果
    std::vector<std::pair<uint32_t, Variant>> results;  // 所有完成的任务结果

    explicit when_any_state(uint32_t n) : needed(n) {}

    /// 任务完成回调。在锁保护下收集结果并检查是否达到需求数。
    /// 返回 true 表示已达到需求数，应恢复调用者。
    ///
    /// Callback on task completion. Returns true if required count met.
    bool on_complete(uint32_t idx, Variant val) {
        std::lock_guard lock(mtx);
        if (resumed) return false; // 已恢复调用者——忽略后续完成
        if (completed == 0) { winner_idx = idx; winner_value = val; }
        results.emplace_back(idx, std::move(val));
        uint32_t prev = completed.fetch_add(1);
        bool done = (prev + 1 >= needed);
        if (done) resumed = true; // 在锁保护下设置，避免双重恢复
        return done;
    }
    bool resumed{false};  // 是否已恢复调用者（防止重复恢复）
};

/// 执行单个 any/some 子任务，完成后回调共享状态。
///
/// Execute one child task for when_any/when_some.
/// TaskIdx: 在参数包中的位置
/// VarIdx:  在 variant 中的位置（跳过了 std::monostate）
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
// 编译期类型工具：
//   - void_filter: 从类型包中移除 void，生成元组
//   - build_variant: 为当 else 构建 variant<std::monostate, Ts...>
//   - non_void_idx: 计算某个位置的类型在非 void 元组中的索引

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
// 并发执行所有任务，收集所有非 void 结果到元组中。
//
// 如果所有任务返回 void，返回空的 std::tuple<>。
// 内部使用 void_state 特化避免不必要的存储开销。
//
// 示例：
//   auto [a, b] = co_await all(task1, task2);
//
// Execute all tasks concurrently, collect non-void results as tuple.
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
// 并发执行所有任务，取第一个完成的结果（类似于 Go 的 select 单分支）。
// 需要至少 2 个任务。
//
// 返回 pair<uint32_t, variant<monostate, Ts...>>：
//   - first:  第一个完成的任务的索引
//   - second: 第一个完成的任务的结果（void 类型使用 monostate 占位）
//
// 示例：
//   auto [idx, val] = co_await any(task1, task2);
//   std::visit(overload{...}, val);
//
// Execute all tasks, take the first completion.
template<typename... TaskTypes>
    requires (sizeof...(TaskTypes) >= 2)
task<std::pair<uint32_t, typename detail::build_variant<typename TaskTypes::value_type...>::type>>
any(TaskTypes... tasks) {
    using variant_t = typename detail::build_variant<typename TaskTypes::value_type...>::type;
    using core_t = detail::when_any_state<variant_t>;
    auto st = std::make_shared<core_t>(1);  // needed = 1（只需一个完成）

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
// 并发执行所有任务，取前 n 个完成的结果（类似于 Go 的 select 多分支）。
// 需要至少 2 个任务。
//
// 返回 vector<pair<uint32_t, variant<monostate, Ts...>>>：
//   按完成顺序排列的前 n 个结果
//
// 示例：
//   auto results = co_await some(2, task1, task2, task3);
//   for (auto& [idx, val] : results) { ... }
//
// Execute all tasks, collect the first N completions.
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
