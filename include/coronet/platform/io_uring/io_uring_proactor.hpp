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

class io_uring_operation {
public:
    explicit io_uring_operation(liburingcxx::sq_entry* sqe) noexcept : sqe_(sqe) {}

    void set_user_data(uint64_t ud) noexcept { if (sqe_) sqe_->set_data(ud); }
    void prepare() noexcept {}  // SQE filled in constructor
    void cancel() noexcept;

    liburingcxx::sq_entry* native_sqe() const noexcept { return sqe_; }

private:
    liburingcxx::sq_entry* sqe_ = nullptr;
};

// ============================================================
// io_uring_proactor (standalone, no virtual base)
// ============================================================

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
    void wakeup() noexcept;

    /// Access the underlying io_uring ring (used by lazy_* structs)
    io_uring_ring& native_ring() noexcept { return ring_; }
    const io_uring_ring& native_ring() const noexcept { return ring_; }

    /// Allocate an SQE (low-level, used by lazy_* awaiter constructors)
    liburingcxx::sq_entry* get_sq_entry() noexcept;

    int poll_completions_impl(void* ctx,
        void (*callback_fn)(void*, const completion_info*)) noexcept;

private:
    io_uring_ring ring_;
    uint32_t entries_ = 0;
    bool initialized_ = false;
    int event_fd_ = -1;           // eventfd for cross-thread wakeup
    uint64_t eventfd_user_data_ = 0;  // marker for eventfd CQEs
    void arm_eventfd();           // submit a pending eventfd read
};

} // namespace coronet::platform::io_uring

#ifdef __GNUC__
#pragma GCC pop_macro("linux")
#endif
