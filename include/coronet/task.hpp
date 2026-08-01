#pragma once

#include <coroutine>
#include <memory>
#include <cassert>
#include <optional>

namespace coronet {
template<typename > class task;
namespace detail {
template<typename> class task_promise_base;
/**
 * @brief When current task<> finishes, resume its parent.
 */
template<typename T>
struct task_final_awaiter
{
    // 总是挂起 -> return false
    // 如果返回 true，直接调用 await_resume()，不挂起
    constexpr bool await_ready() const noexcept { return false; }

    // C++20 标准规定 await_suspend 可以返回三种类型：
    // void -> 挂起当前协程，不指定恢复目标
    // std::coroutine_handle<> -> 立即恢复到指定协程
    // bool -> true=挂起，false=继续执行
    /*
     *  为什么选择返回 coroutine_handle<>？
        这样做的好处：
        零开销切换：直接跳转到父协程，无需经过调度器
        同步执行语义：子协程在父协程的栈帧上"内联"执行
        避免额外挂起：不会增加调度器的负担
     */
    template<std::derived_from<task_promise_base<T>> Promise>
    constexpr std::coroutine_handle<>
    await_suspend(std::coroutine_handle<Promise> current) noexcept {
        return current.promise().parent_coroutine;
    }

    // 因为 await_suspend 已经返回了父协程句柄，控制权直接转移
    // 永远不会调用这个函数
    // Won't be resumed anyway
    constexpr void await_resume() const noexcept {}
};
template<>
struct task_final_awaiter<void>
{
    // 对于已分离（detached）的 task<void>，await_ready() 返回 true，
    // 协程在 final_suspend 点不挂起，由 C++20 运行时自动销毁帧。
    // 这避免了在 await_suspend 内部调用 current.destroy()，
    // 因为 MSVC 的运行时在 await_suspend 返回后仍会访问协程帧。
    //
    // For detached task<void>, await_ready() returns true so the
    // runtime destroys the frame automatically, avoiding the need
    // to call current.destroy() inside await_suspend (which MSVC's
    // runtime does not support — it accesses the frame afterwards).
    //
    // P2-1 fix: 从 promise 显式注入 is_detached_（成员默认初始化 {false}，
    // 确定性无垃圾值）。P0-2 曾因 MSVC 帧内 flag 未初始化误判 detached，
    // 移除了检查导致所有 detached 帧在 final_suspend 挂起后句柄被 detach()
    // 清空，帧永久孤儿化泄漏（C1000K 每连接 +4.4KB，三端一致）。
    // 构造函数注入保证 flag 与 promise 状态同步，重新启用自动销毁。
    explicit constexpr task_final_awaiter(bool is_detached) noexcept
        : is_detached_(is_detached) {}

    constexpr bool await_ready() const noexcept {
        return is_detached_;
    }
    template<std::derived_from<task_promise_base<void>> Promise>
    constexpr std::coroutine_handle<>
    await_suspend(std::coroutine_handle<Promise> current) const noexcept {
        // 仅非分离任务到达此处（分离任务已通过 await_ready=true 路径处理）
        // Only non-detached tasks reach here
        return current.promise().parent_coroutine;
    }
    constexpr void await_resume() const noexcept {}

private:
    bool is_detached_;
};

/**
 * @brief Define the behavior of all tasks.
 *
 * final_suspend: yes, and return to parent
 */
template<typename T>
class task_promise_base
{
    friend struct task_final_awaiter<T>;
public:
    task_promise_base() noexcept = default;

    // 协程的"启动按钮"
    // Lazy: suspend_always → 等待被 co_await 时才执行
    // DBG: 加日志验证 initial_suspend 被调用
    constexpr std::suspend_always initial_suspend() const noexcept {
        return {};
    }

    // 协程的"返回机制"
    // 返回定制的 task_final_awaiter
    // task_final_awaiter 负责恢复父协程
    constexpr task_final_awaiter<T> final_suspend() const noexcept {
        return {};
    }

    // 建立"调用链"
    constexpr void set_parent(std::coroutine_handle<> continuation) noexcept {
        parent_coroutine = continuation;
    }

    /*
     * 为什么禁止拷贝和移动？
     * 原因 1：Promise 与协程帧绑定
     * // 协程帧的内存布局
        ┌─────────────────────────────┐
        │ Coroutine Frame             │
        │ ├─ Promise (task_promise)   │ ← 不能独立于帧存在
        │ ├─ Coroutine State          │
        │ └─ Local Variables          │
        └─────────────────────────────┘

        task_promise<int> p1;
        auto p2 = p1;  // ❌ 浅拷贝 vs 深拷贝？

        // 浅拷贝问题：两个 promise 指向同一个协程帧
        // 深拷贝问题：无法复制协程的执行状态（寄存器、局部变量等）


     * 原因 2：句柄的唯一性
     * // coroutine_handle 本质是指针
        std::coroutine_handle<promise_type> handle;

        // 如果 promise 被移动，handle 会失效
        task_promise<int> p1;
        auto handle = std::coroutine_handle<task_promise<int>>::from_promise(p1);
        auto p2 = std::move(p1);  // ❌ handle 现在指向哪里？

     *
     * 原因 3：RAII 语义
     * // Promise 负责管理协程资源
        ~task_promise() {
            if (state == value_state::value) {
                value.~T();  // ← 析构时清理资源
            }
        }
        // 如果允许多个 promise 共享资源，会导致双重释放
     */
    task_promise_base(const task_promise_base &) = delete;
    task_promise_base(task_promise_base &&) = delete;
    task_promise_base &operator=(const task_promise_base &) = delete;
    task_promise_base &operator=(task_promise_base &&) = delete;

private:
    // 默认初始化为 空转的协程，resume() 什么都不做
    // 如果是 nullptr, 调用 resume() 会崩溃
    std::coroutine_handle<> parent_coroutine{std::noop_coroutine()};
};

/**
 * @brief task<> with a return value
 * @tparam T the type of the final result
 */
template<typename T>
class task_promise final : public task_promise_base<T>
{
public:
    task_promise() noexcept: state(value_state::mono) {};

    ~task_promise() {
        switch (state) {
            [[likely]] case value_state::value:
                value.~T();
                break;
            case value_state::exception:
                exception_ptr.~exception_ptr();
                break;
            default:
                break;
        }
    };

    /*
     * get_return_object() 是 C++20 协程机制中连接 Promise 与外部世界的桥梁，它的作用可以概括为：
     * 在协程开始执行前，从 Promise 对象创建一个可被外部使用的句柄对象（Task/Generator/Future 等）
     */
    // 要理解 get_return_object() 的作用，必须先了解编译器生成的协程创建代码：
    /*
     * 用户代码：
     task<int> compute() {
        co_return 42;
     }
     * 编译器展开后的伪代码：
     task<int> compute() {
        // ========== 阶段 1：分配协程帧 ==========
        void* memory = operator new(sizeof(coroutine_frame));

        // ========== 阶段 2：构造 Promise ==========
        task_promise<int>* promise = new (memory) task_promise<int>();

        // ========== 阶段 3：调用 get_return_object() ==========
        // ← 这里！在协程体执行前就返回 task 对象
        task<int> return_obj = promise->get_return_object();

        // ========== 阶段 4：调用 initial_suspend() ==========
        co_await promise->initial_suspend();
        // ↑ suspend_always → 立即挂起，等待被 co_await

        // ========== 阶段 5：执行协程体 ==========
        // ... 这部分代码要等到被 co_await 时才执行
        promise->return_value(42);

        // ========== 阶段 6：final_suspend ==========
        co_await promise->final_suspend();

        return return_obj;  // ← 实际上早就返回了
    }

     关键发现：
     ┌──────────────────────────────────────┐
     │  important!                          │
     │                                      │
     │  get_return_object() 在以下时机调用： │
     │  ✓ Promise 构造之后                  │
     │  ✓ initial_suspend 之前              │
     │  ✓ 协程体执行之前                    │
     │                                      │
     │  这意味着：                          │
     │  • task 对象返回时，协程还未开始执行 │
     │  • 这就是 Lazy（惰性）的关键         │
     └──────────────────────────────────────┘


     */

    task<T> get_return_object() noexcept ;

    // 异常捕获机制
    void unhandled_exception() noexcept {
        // 必须用 construct_at：exception_ptr 与 value 在同一 union 中，
        // 构造时 union 成员未初始化，直接 operator= 会尝试释放旧值（垃圾指针）。
        // Must use construct_at: exception_ptr shares a union with value,
        // which is uninitialized at construction. Direct assignment would
        // invoke exception_ptr's operator= and attempt to release the
        // old (garbage) value — crash on MSVC.
        std::construct_at(std::addressof(exception_ptr), std::current_exception());
        state = value_state::exception;
    }

    //  处理 co_return
    template<typename Value>
    requires std::convertible_to<Value &&, T>
    //          ↑ C++20 concept
    // 接受任何可以转换为 T 的类型
    // 万能引用与约束
    void return_value(Value &&result)
    noexcept(std::is_nothrow_constructible_v<T, Value &&>) {
        //       ↑ 取决于 T 的构造函数是否抛异常
        std::construct_at(
                // placement new（C++20 简化为 construct_at）
                // 等价于：new (&value) T(std::forward<Value>(result));
                std::addressof(value), std::forward<Value>(result)
                         );
        state = value_state::value;
    }

    // get the lvalue ref
    T &result() & {
        if (state == value_state::exception) [[unlikely]] {
            std::rethrow_exception(exception_ptr);
        }
        assert(state == value_state::value);
        if (state != value_state::value) [[unlikely]] {
            std::terminate();
        }
        return value;
    }

    // get the prvalue
    T &&result() && {
        if (state == value_state::exception) [[unlikely]] {
            std::rethrow_exception(exception_ptr);
        }
        assert(state == value_state::value);
        if (state != value_state::value) [[unlikely]] {
            std::terminate();
        }
        return std::move(value);
    }

private:
    union
    {
        T value;
        std::exception_ptr exception_ptr;
    };
    // 状态 含义 触发时机
    // mono -> 初始状态（monomorphic）-> 构造完成后
    // value -> 已成功返回值 -> co_return x; -> 执行后
    // exception -> 发生异常 -> unhandled_exception() -> 被调用后
    enum class value_state : uint8_t { mono, value, exception } state;
    /*
     * 为什么需要 mono 状态？
     * 作用：区分"未初始化"和"已初始化但为空"
     * 例如：task<int> 可能返回 0，但这不是默认值
     * 状态转换图：
     * 构造 → mono
                 ↓
            ┌────┴────┐
            │         │
        co_return   throw
            │         │
            ↓         ↓
          value   exception

     */
};

template<>
class task_promise<void> final : public task_promise_base<void>
{
    // 友元声明： 便于task_final_awaiter<void>::await_suspend 访问detached标志
    friend struct task_final_awaiter<void>;
    // 友元声明：便于task<void>::detached() 访问detached 标志
    friend class task<void>;

public:
    task_promise() noexcept = default;
    ~task_promise() noexcept = default;

    task<void> get_return_object() noexcept;

    constexpr void return_void() const noexcept {}

    // P2-1 fix: final_suspend 从 promise 的 is_detached_（成员默认初始化
    // {false}，无垃圾值）构造 final awaiter，重新启用 detached 帧的运行时
    // 自动销毁。P0-2 曾因 MSVC 帧内 flag 未初始化误判 detached（when_all
    // void 路径死锁）而移除检查，代价是 detached 帧在 final_suspend 挂起后
    // 句柄被 detach() 清空 → 帧永久孤儿化泄漏（C1000K 每连接 +4.4KB）。
    // 构造函数注入保证确定性初始化，且 final_awaiter 不再有独立默认成员。
    constexpr task_final_awaiter<void> final_suspend() const noexcept {
        return task_final_awaiter<void>{is_detached_};
    }

    // P0-2 fix: 不在 unhandled_exception 中 rethrow（即使 is_detached_ 为 true）。
    // 旧实现在 is_detached_ 为 true 时 rethrow，但 is_detached_ 可能在协程帧
    // 未正确初始化时为垃圾值，导致无限 rethrow → 栈溢出 → STATUS_STACK_BUFFER_OVERRUN。
    // 新实现：总是存储异常，由调用方通过 result() 决定是否 rethrow。
    void unhandled_exception() noexcept {
        exception_ptr_.emplace(std::current_exception());
    }

    void result() const {
        if (exception_ptr_) [[unlikely]] {
            std::rethrow_exception(*exception_ptr_);
        }
    }

private:
    // P0-2 fix: 用 std::optional 管理 exception_ptr 生命周期。
    // 直接用 std::exception_ptr 作为 promise 成员在 MSVC 协程帧中
    // 可能导致帧布局异常。std::optional 延迟构造 exception_ptr。
    bool is_detached_{false};
    std::optional<std::exception_ptr> exception_ptr_;
};

/**
 * 根据所有权语义，引用是借用资源，所以无需对借用的资源负责清理，编译器默认的析构函数是合适的。
 */
template<typename T>
class task_promise<T &> final : public task_promise_base<T &>
{
public:
    task_promise() noexcept = default;

    task<T &> get_return_object() noexcept;

    void unhandled_exception() noexcept {
        this->exception_ptr = std::current_exception();
    }

    // 当协程执行 co_return ref; 时，
    // 编译器自动调用 return_value(ref)，将引用的地址保存到 promise 中
    /*
     * 完整的调用时机
     int global = 42;

     task<int&> get_ref() {
         co_return global;  // ← 这里！
     }

     编译器实际生成的伪代码：
     task<int&> get_ref() {
     // ========== 阶段 1：分配协程帧 ==========
     void* memory = operator new(sizeof(coroutine_frame));

     // ========== 阶段 2：构造 Promise ==========
     task_promise<int&>* promise = new (memory) task_promise<int&>();

     // ========== 阶段 3：调用 get_return_object() ==========
     task<int&> return_obj = promise->get_return_object();

     // ========== 阶段 4：initial_suspend ==========
     co_await promise->initial_suspend();
     // ↑ suspend_always → 立即挂起

     // ========== 阶段 5：执行协程体 ==========
     // ↓↓↓ 关键！编译器将 co_return 翻译为 ↓↓↓
     promise->return_value(global);
     //                              ↑ 调用这里！

     // ========== 阶段 6：final_suspend ==========
     co_await promise->final_suspend();

     return return_obj;
    }


     */
    // C++20 标准 [coroutine.promise] 规定：
    // promise.return_value(...) 必须是 noexcept 的

    // 原因：
    // 如果 return_value 抛异常，会导致：
    // 1. 协程处于"正在返回但未完成"的不确定状态
    // 2. 无法确定是否应该调用 unhandled_exception()
    // 3. 破坏协程的状态机

    void return_value(T &result) noexcept {
        value = std::addressof(result);
    }

    T &result() const {
        if (exception_ptr) [[unlikely]] {
            std::rethrow_exception(exception_ptr);
        }
        return *value;
    }

private:
    T *value{nullptr};
    std::exception_ptr exception_ptr;
};
}



template<typename T = void>
class [[nodiscard("Did you forget to co_await?")]] task
{
public:
    /**
     * ---------------------------------------------------
     * task 类型成员
     * ---------------------------------------------------
     */
    // 协成必须持有的 promise_type 类型
    using promise_type = detail::task_promise<T>;
    using value_type = T;

    // 协成 concept 标记
    struct is_task_like {};
    /**
     * ---------------------------------------------------
     * task 构造
     * ---------------------------------------------------
     */
    task() noexcept = default;

    explicit task(std::coroutine_handle<promise_type> current) noexcept
            : handle(current) {}

    task(task &&other) noexcept: handle(other.handle) {
        other.handle = nullptr;
    }

    task &operator=(task &&other) noexcept {
        if (this != std::addressof(other)) [[likely]] {
            if (handle) {
                handle.destroy();
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }

    // 禁用copy构造，copy赋值
    // 唯一所有权语义 (可移动，不可复制)
    task(const task &)  =  delete;
    task &operator=(const task &) = delete;

    // 析构，释放 promise 对象和协成参数
    // Free the promise object and coroutine parameters
    ~task() noexcept {
        if (handle) {
            handle.destroy();
        }
    }
    /**
     * ---------------------------------------------------
     * task 功能性API
     * ---------------------------------------------------
     */
     [[nodiscard("is the task done, or invalided?")]]
     bool is_ready() const noexcept {
         return !handle || handle.done();
     }

    /*
        1. 调用 detach()
           ↓
        2. 设置标志：is_detached_flag = is_detached (-1ULL)
           ↓
        3. 清空句柄：handle = nullptr
           ↓
        4. 子协程继续独立运行...
           ↓
        5. 协程执行完毕
           ↓
        6. final_suspend() → task_final_awaiter<void>
           ↓
        7. await_ready() 检查：is_detached_flag == is_detached ✅
           ↓
        8. await_ready() 返回 true → 运行时自动销毁帧
           （不在 await_suspend 中调用 current.destroy()，
            因为 MSVC 运行时在 await_suspend 返回后仍访问协程帧）
     */
    // 直接销毁协程资源，不返回到父协程（因为父协程可能已经不存在了）
    // 将任务与调用者解耦，让任务"独立运行"，调用者不等待其结果，也不负责清理资源。
    //
    // 仅 task<void> 支持 detach：void 任务无返回值需要消费，
    // 分离后协程在 final_suspend 时由运行时自动销毁帧。
    // task<T>（非 void）有返回值必须被消费，detach 会导致帧泄漏，
    // 因此用 requires 约束在编译期禁止。
    void detach() noexcept requires std::is_void_v<T> {
        handle.promise().is_detached_ = true;
        handle = nullptr;
    }

    [[nodiscard]]
    std::coroutine_handle<promise_type> get_handle() const noexcept {
        return handle;
    }

    friend void swap(task &a, task &b) noexcept {
        std::swap(a.handle, b.handle);
    }

    /*
       调用 when_ready() 时的协程状态机：

       caller() 协程
          ↓
       co_await t.when_ready()
          ↓
       1. await_ready() → false (任务未完成)
          ↓
       2. await_suspend(caller_handle)
          - 设置 parent_coro = caller_handle
          - 返回 task_handle
          ↓
       3. 执行 task 协程体
          ↓
       4. task 执行完毕 → final_suspend()
          ↓
       5. task_final_awaiter::await_suspend()
          - 返回 parent_coro (caller_handle)
          ↓
       6. 恢复到 caller 协程
          ↓
       7. await_resume() (空函数)
          ↓
       8. 继续执行 caller 后续代码
     */
    /**
     * 只需要等待执行完成：某些场景下，调用者只关心任务何时完成，而不需要任务的返回值
     * 解耦执行与结果消费：将"等待完成"和"获取结果"两个操作分离，提供更灵活的 API
     * 避免异常重复抛出：如果多次 co_await task，每次都会重新抛出异常（如果有），而 when_ready() 只等待不抛异常
     *
     * ready: adv. 已做完；已完成
     * @brief wait for the task<> to complete, but do not get the result
     */
    [[nodiscard("did you forget co_await?")]]
    auto when_ready() const noexcept {
        struct awaiter : awaiter_base
        {
            using awaiter_base::awaiter_base;
            constexpr void await_resume() const noexcept {}
        };
        return awaiter{handle};
    }

    /*
    co_await expression;  // 编译器会将其展开为：
    编译器实际执行的伪代码：

    auto&& awaitable = expression;              // 1. 获取可等待对象
    auto&& awaiter = get_awaiter(awaitable);    // 2. 获取 awaiter

    if (!awaiter.await_ready()) {               // 3. 检查是否需要挂起
        suspend_coroutine();                    // 4. 挂起当前协程

        // ... 协程暂停，执行权交还给调度器 ...

        awaiter.await_suspend(current_handle);  // 5. 注册恢复回调
    }

    awaiter.await_resume();                     // 6. 恢复后获取结果



    调用 co_await task 时的控制流：

    caller() 协程                          task 协程
       │                                     │
       ├─ await_ready() → false              │
       │   (需要挂起)                        │
       │                                     │
       ├─ await_suspend(caller_handle)       │
       │   │                                 │
       │   ├─ set_parent(caller_handle)      │
       │   │   (记录"谁在等我")              │
       │   │                                 │
       │   └─ return task_handle             │
       │       (立即切换到 task)             │
       │                                     │
       │                                     ▼ 开始执行
       │                              执行协程体...
       │                              co_return 42;
       │                                     │
       │                                     ▼ final_suspend
       │                              task_final_awaiter
       │                                     │
       │                                     ├─ await_suspend()
       │                                     │   return parent_coro
       │                                     │   (返回 caller_handle)
       │                                     │
       ◄─────────────────────────────────────┘
       恢复到 caller
       │
       └─ await_resume() → 42
           (获取结果)

     */
    /**
     * @brief wait for the task<> to complete, and get the ref of the result
     */
    auto operator co_await() const & noexcept {
        struct awaiter : awaiter_base
        {
            using awaiter_base::awaiter_base;

            decltype(auto) await_resume() {
                assert(this->handle_ && "broken_promise");
                return this->handle_.promise().result();
            }
        };
        return awaiter{handle};
    }

    /**
     * @brief wait for the task<> to complete, and get the rvalue ref of the
     * result
     */
    auto operator co_await() const && noexcept {
        struct awaiter : awaiter_base
        {
            using awaiter_base::awaiter_base;

            decltype(auto) await_resume() {
                assert(this->handle_ && "broken_promise");
                return std::move(this->handle_.promise()).result();
            }
        };
        return awaiter{handle};
    }

private:
    // awaitable for operator co_await
    struct awaiter_base
    {
        explicit awaiter_base(std::coroutine_handle<promise_type > current)
            noexcept : handle_(current) {}
        [[nodiscard]]
        constexpr bool await_ready() const noexcept {
            // null handle → false → fall through to await_suspend → terminate
            // (NOT true, which would skip to await_resume and dereference null)
            return handle_ && handle_.done();
        }
        constexpr auto await_suspend(std::coroutine_handle<> awaiting) noexcept {
            if (!handle_) [[unlikely]] {
                std::terminate();
            }
            handle_.promise().set_parent(awaiting);
            return handle_;
        }

        std::coroutine_handle<promise_type > handle_;
    };

private:
    std::coroutine_handle<promise_type> handle;
};


namespace detail {
template<typename T>
inline task<T> task_promise<T>::get_return_object() noexcept {
    return task<T>{std::coroutine_handle<task_promise>::from_promise(*this)};
}

inline task<void> task_promise<void>::get_return_object() noexcept {
    return task<void>{std::coroutine_handle<task_promise>::from_promise(*this)};
}

template<typename T>
inline task<T &> task_promise<T &>::get_return_object() noexcept {
    return task<T &>{std::coroutine_handle<task_promise>::from_promise(*this)};
}

}

}