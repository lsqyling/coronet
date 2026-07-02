#pragma once

#ifdef __GNUC__
#pragma GCC push_macro("linux")
#undef linux
#endif

#include "coronet/config/uring.hpp"
#include "coronet/platform/platform.hpp"

#include <uring/uring.hpp>

#include <memory>

namespace coronet::platform::io_uring {

/// The concrete io_uring type with kernel-version-dependent flags
// 实际的 io_uring 类型，根据内核版本启用不同特性标志
// io_uring 是 Linux 5.1 引入的异步 I/O 框架，采用共享 ring buffer 在用户态和内核态之间传递 I/O 请求（SQE）和完成事件（CQE）。
// 这里的类型别名根据编译时检测的内核版本，逐步启用更高级的特性：
//   - 5.19+: IORING_SETUP_COOP_TASKRUN — 协作式 taskrun，减少跨核唤醒开销
//   - 6.0+:  IORING_SETUP_SINGLE_ISSUER — 单线程提交优化，省去内部锁
//   - 6.1+:  IORING_SETUP_DEFER_TASKRUN — 延迟 taskrun，批量处理完成事件，进一步提升吞吐
// 这样设计的好处是：一份代码自适应不同内核版本，运行时无需条件判断（编译期静态选择）。
using io_uring_ring = liburingcxx::uring<
    config::io_uring_setup_flags
    | config::uring_setup_flags
#if LIBURINGCXX_IS_KERNEL_REACH(5, 19)
    | config::io_uring_coop_taskrun_flag
#endif
#if LIBURINGCXX_IS_KERNEL_REACH(6, 0)
    | IORING_SETUP_SINGLE_ISSUER
#endif
#if LIBURINGCXX_IS_KERNEL_REACH(6, 1)
    | IORING_SETUP_DEFER_TASKRUN
#endif
>;

// ============================================================
// io_uring_operation — wraps an SQE (standalone, no virtual base)
// ============================================================
// io_uring_operation — 封装一个 SQE 条目（无虚函数，独立类型）
// io_uring 模型是 completion-based（基于完成）的：用户提交 SQE 到提交队列，内核完成后将 CQE 放入完成队列。
// 这与 epoll 的 readiness-based（基于就绪）模型不同：
//   - epoll:  内核通知"fd 就绪"，用户态自行执行 read/write syscall
//   - io_uring:内核直接执行 I/O，用户态从 CQE 拿到结果，省去了一次 syscall
// 因此 io_uring_operation 本质上只是 SQE 的轻量包装：提交时填充 SQE，完成后从 CQE 读取结果。
// 不包含虚函数，由上层 proactor_concept 通过 compile-time 多态调用。

class io_uring_operation {
public:
    explicit io_uring_operation(liburingcxx::sq_entry* sqe) noexcept : sqe_(sqe) {}

    void set_user_data(uint64_t ud) noexcept { if (sqe_) sqe_->set_data(ud); }
    void prepare() noexcept {}  // SQE filled in constructor
    // SQE 在构造函数中已填充完毕，prepare 为空操作
    void cancel() noexcept;

    liburingcxx::sq_entry* native_sqe() const noexcept { return sqe_; }

private:
    liburingcxx::sq_entry* sqe_ = nullptr;
};

// ============================================================
// io_uring_proactor (standalone, no virtual base)
// ============================================================
// io_uring_proactor — io_uring 驱动的 Proactor（无虚函数，独立类型）
//
// 核心职责：
//   1. 管理 io_uring 实例的生命周期（init / deinit）
//   2. 提供 SQE 分配接口（get_sq_entry / acquire_operation）
//   3. 提交 SQE 到内核（submit）
//   4. 收割 CQE 完成事件（wait_completion / poll_completions_impl）
//   5. 跨线程唤醒（eventfd + arm_eventfd）
//
// 为什么选择 compile-time 多态而非虚函数：
//   - 每个 Proactor（io_uring / IOCP / epoll）都满足相同的隐式 proactor_concept
//   - platform.hpp 中通过 #ifdef 在编译期选择具体类型
//   - 避免了虚函数调用的开销（函数指针间接跳转），对高性能 I/O 至关重要
//
// io_uring 的管理模式：
//   - SQE（Submission Queue Entry）：提交队列，用户填充 I/O 请求，内核消费
//   - CQE（Completion Queue Entry）：完成队列，内核写入 I/O 结果，用户消费
//   - SQ 和 CQ 是环形缓冲区（ring buffer），通过 mmap 映射到用户态，避免了 syscall 的数据拷贝
//   - submit() 调用 io_uring_enter syscall 通知内核处理 SQE
//   - 支持批量提交/批量收割，减少上下文切换次数
//
// 跨线程唤醒机制（eventfd）：
//   - 当另一个线程通过 co_spawn 提交协程到本线程时，需要唤醒阻塞在 wait_completion 的事件循环
//   - eventfd 是一个轻量的内核事件通知机制，写入 1 即可触发可读事件
//   - 将 eventfd 注册为 io_uring 的读操作（prep_read），当 eventfd 可读时，CQE 中会带有 eventfd_user_data_ 标记
//   - wait_completion 检测到这个标记后，返回 0 通知调用者排空跨线程队列，然后重新 arm eventfd

class io_uring_proactor {
public:
    using operation_type = io_uring_operation;

    io_uring_proactor() noexcept = default;
    ~io_uring_proactor() noexcept { deinit(); }

    io_uring_proactor(const io_uring_proactor&) = delete;
    io_uring_proactor& operator=(const io_uring_proactor&) = delete;
    io_uring_proactor(io_uring_proactor&&) = delete;
    io_uring_proactor& operator=(io_uring_proactor&&) = delete;

    void init(uint32_t entries);
    void deinit() noexcept;

    std::unique_ptr<io_uring_operation> acquire_operation();

    int  submit(bool wait = false) noexcept;
    int  wait_completion(completion_info* info) noexcept;
    intptr_t native_handle() const noexcept;

    /// Wake up a blocked wait_completion() via eventfd for cross-thread co_spawn.
    // 通过 eventfd 唤醒阻塞的 wait_completion()，用于跨线程 co_spawn。
    void wakeup() noexcept;

    /// Access the underlying io_uring ring (used by lazy_* structs)
    // 获取底层 io_uring ring 的引用（用于 lazy_* awaiter 结构体）
    io_uring_ring& native_ring() noexcept { return ring_; }
    const io_uring_ring& native_ring() const noexcept { return ring_; }

    /// Allocate an SQE (low-level, used by lazy_* awaiter constructors)
    // 分配一个 SQE（底层接口，供 lazy_* awaiter 构造函数使用）
    liburingcxx::sq_entry* get_sq_entry() noexcept;

    int poll_completions_impl(void* ctx,
        void (*callback_fn)(void*, const completion_info*)) noexcept;

private:
    io_uring_ring ring_;
    uint32_t entries_ = 0;
    bool initialized_ = false;
    int event_fd_ = -1;           // eventfd for cross-thread wakeup
    // eventfd 文件描述符，用于跨线程唤醒
    uint64_t eventfd_user_data_ = 0;  // marker for eventfd CQEs
    // eventfd 对应的 CQE 标记值，用于在 wait_completion 中识别 eventfd 唤醒事件
    void arm_eventfd();           // submit a pending eventfd read
    // 提交一个 eventfd 的读请求到 io_uring，使其在 eventfd 可读时产生 CQE
};

} // namespace coronet::platform::io_uring

#ifdef __GNUC__
#pragma GCC pop_macro("linux")
#endif
