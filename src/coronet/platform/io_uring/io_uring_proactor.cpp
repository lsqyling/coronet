#ifdef __GNUC__
#pragma GCC push_macro("linux")
#undef linux
#endif

#include "coronet/platform/io_uring/io_uring_proactor.hpp"
#include "coronet/log/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <sys/eventfd.h>
#include <unistd.h>

namespace coronet::platform::io_uring {

void io_uring_proactor::init(uint32_t entries) {
    if (initialized_) return;
    log::d("[uring] init(entries=%u)\n", entries);
    entries_ = entries;
    ring_.init(entries);
    log::d("[uring] init done, fd=%d\n", ring_.fd());

    // Create eventfd for cross-thread wakeup
    // 创建 eventfd 用于跨线程唤醒
    // eventfd 是 Linux 提供的一种轻量级事件通知机制，可以在进程内部或进程之间传递事件信号。
    // 在这里的用途是：当另一个线程通过 cross_thread_queue 提交协程到本线程时，
    // 需要唤醒阻塞在 wait_completion() 上的事件循环，使其排空跨线程队列。
    // 使用 EFD_NONBLOCK 避免在 drain 时阻塞，EFD_CLOEXEC 防止 fork 后泄漏 fd。
    event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd_ >= 0) {
        eventfd_user_data_ = static_cast<uint64_t>(-1); // marker
        // 使用全 1 作为标记值，这样在 CQE 中通过 user_data 可以唯一识别 eventfd 事件
        arm_eventfd();
        log::d("[uring] eventfd=%d armed\n", event_fd_);
    }

    initialized_ = true;
}

void io_uring_proactor::arm_eventfd() {
    if (event_fd_ < 0) return;
    auto* sqe = ring_.get_sq_entry();
    if (!sqe) return;
    // 提交一个 eventfd 的读请求到 io_uring。
    // 当 eventfd 被写入数据（wakeup 时），内核完成这个读操作，产生 CQE。
    // 这样就可以在 wait_completion 中统一处理 I/O 完成和跨线程唤醒，不用额外的 epoll/poll。
    // dummy 是一个栈上变量，但在实际使用中 eventfd 的读操作是异步的，
    // 内核在读取时不需要保留用户缓冲区（eventfd 数据在内核中），这里只是为了占位。
    static uint64_t dummy = 0;
    sqe->prep_read(event_fd_, std::span<char>{reinterpret_cast<char*>(&dummy), sizeof(dummy)}, 0);
    sqe->set_data(eventfd_user_data_);
    ring_.submit();
}

void io_uring_proactor::deinit() noexcept {
    if (initialized_) {
        if (event_fd_ >= 0) {
            ::close(event_fd_);
            event_fd_ = -1;
        }
        // 显式调用析构函数然后 placement new 重建，相当于 clear + reset 的效果
        ring_.~io_uring_ring();
        std::construct_at(&ring_);
        initialized_ = false;
    }
}

liburingcxx::sq_entry* io_uring_proactor::get_sq_entry() noexcept {
    auto* sqe = ring_.get_sq_entry();
    if (!sqe) {
        log::e("[uring] get_sq_entry() returned NULL — SQ ring full!\n");
        // SQE 分配失败通常意味着提交队列已满，需要先 submit 释放 SQE 槽位。
        // 这种情况在突发 I/O 量过大时可能发生，上层应进行背压处理。
    }
    return sqe;
}

std::unique_ptr<io_uring_operation> io_uring_proactor::acquire_operation() {
    // 分配一个 SQE 并包装为 io_uring_operation。
    // 这个接口主要用于满足 proactor_concept 的 operation_type 要求，
    // 但对于 io_uring 来说，lazy_* awaiter 通常直接调用 get_sq_entry() 以避免 unique_ptr 的堆分配开销。
    auto* sqe = ring_.get_sq_entry();
    if (!sqe) return nullptr;
    return std::make_unique<io_uring_operation>(sqe);
}

int io_uring_proactor::submit(bool wait) noexcept {
    log::v("[uring] submit(wait=%d)\n", wait);
    // submit_and_wait 封装了 io_uring_enter syscall，将 SQE 提交给内核处理。
    // wait=true 时，内核会等待至少一个完成事件后再返回（相当于阻塞式提交）。
    // wait=false 时，内核处理完 SQE 立即返回，不等待完成。
    int ret = ring_.submit_and_wait(wait ? 1 : 0);
    if (ret < 0) {
        // EAGAIN 表示内核无法立即处理，EINTR 表示被信号中断，这两种情况属于正常重试场景
        if (ret == -EAGAIN || ret == -EINTR) return 0;
        log::e("[uring] submit() failed: %s\n", std::strerror(-ret));
    }
    log::v("[uring] submit returned %d\n", ret);
    return ret;
}

int io_uring_proactor::wait_completion(completion_info* info) noexcept {
    // wait_completion 的核心流程：
    //   1. 尝试从 CQ（完成队列）中 peek 一个 CQE
    //   2. 如果队列为空，则调用 submit(true) 阻塞等待内核处理并返回完成事件
    //   3. 从 CQE 中提取结果（user_data, res, flags）
    //   4. 如果是 eventfd 的 CQE，则重新 arm eventfd 并返回 0 通知上层排空跨线程队列
    //   5. 如果不是，则填充 completion_info 并前进 CQ 游标
    //
    // 这种设计实现了 "提交-等待" 一体化：一次函数调用既提交了之前的 SQE，又等待了完成事件。
    // 减少了 syscall 次数，提高了吞吐。
    const liburingcxx::cq_entry* cqe_ptr = nullptr;
    int ret = ring_.peek_cq_entry(cqe_ptr);
    log::v("[uring] wait_completion: peek=%d cqe=%p\n", ret, (void*)cqe_ptr);
    if (ret != 0 || cqe_ptr == nullptr) {
        log::v("[uring] no CQE ready, calling submit(true)\n");
        submit(true);
        ret = ring_.peek_cq_entry(cqe_ptr);
        log::v("[uring] after submit+peek: ret=%d cqe=%p\n", ret, (void*)cqe_ptr);
        if (ret != 0 || cqe_ptr == nullptr) return 0;
    }

    // Check if this is the eventfd notification (cross-thread wakeup)
    // 检查是否为 eventfd 的 CQE（跨线程唤醒信号）
    if (cqe_ptr->user_data == eventfd_user_data_) {
        log::v("[uring] eventfd CQE — draining cross-thread queue\n");
        ring_.cq_advance(1);
        arm_eventfd(); // re-arm for next wakeup
        // 重新 arming 以备下一次跨线程唤醒
        return 0;      // caller will drain cross-thread queue
        // 返回 0 通知调用者去排空跨线程协程队列
    }

    info->user_data = cqe_ptr->user_data;
    info->result = cqe_ptr->res;
    info->flags = cqe_ptr->flags;
    info->opaque = nullptr;
    log::v("[uring] CQE: user_data=%llu res=%d flags=%u\n",
           (unsigned long long)cqe_ptr->user_data, cqe_ptr->res, cqe_ptr->flags);
    ring_.cq_advance(1);
    return 1;
}

void io_uring_proactor::wakeup() noexcept {
    // 跨线程唤醒：向 eventfd 写入数据。
    // eventfd 的计数器值增加后变为可读状态，内核会完成之前 arm_eventfd 提交的读请求，
    // 从而产生 CQE，解除 wait_completion 的阻塞。
    // 这样，目标线程的事件循环就能感知到有新的协程需要调度。
    if (event_fd_ >= 0) {
        uint64_t val = 1;
        (void)::write(event_fd_, &val, sizeof(val));
    }
}

intptr_t io_uring_proactor::native_handle() const noexcept {
    return ring_.fd();
}

void io_uring_operation::cancel() noexcept {
    // 将 SQE 指针置空，标记此操作已被取消。
    // 注意：io_uring 中真正的取消需要提交 IORING_OP_ASYNC_CANCEL 类型的 SQE，
    // 但这里只是标记防止后续使用，实际 SQE 已经提交给内核了。
    sqe_ = nullptr;
}

int io_uring_proactor::poll_completions_impl(
    void* ctx, void (*callback_fn)(void*, const completion_info*)) noexcept
{
    // 批量收割所有可用的完成事件。
    // 与 wait_completion 每次只处理一个 CQE 不同，这个接口会循环处理所有可用的 CQE，
    // 并对每个 CQE 调用 callback_fn 进行回调处理。
    // 这在事件循环的批量处理阶段非常有用，可以减少 CQE 收割的开销。
    int count = 0;
    const liburingcxx::cq_entry* cqe_ptr = nullptr;
    while (ring_.peek_cq_entry(cqe_ptr) == 0 && cqe_ptr != nullptr) {
        if (cqe_ptr->user_data == eventfd_user_data_) {
            ring_.cq_advance(1);
            arm_eventfd();
            cqe_ptr = nullptr;
            continue;
        }
        completion_info info{};
        info.user_data = cqe_ptr->user_data;
        info.result = cqe_ptr->res;
        info.flags = cqe_ptr->flags;
        callback_fn(ctx, &info);
        ++count;
        ring_.cq_advance(1);
        cqe_ptr = nullptr;
    }
    return count;
}

} // namespace coronet::platform::io_uring

#ifdef __GNUC__
#pragma GCC pop_macro("linux")
#endif
