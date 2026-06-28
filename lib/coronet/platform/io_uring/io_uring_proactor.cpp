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
    event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd_ >= 0) {
        eventfd_user_data_ = static_cast<uint64_t>(-1); // marker
        arm_eventfd();
        log::d("[uring] eventfd=%d armed\n", event_fd_);
    }

    initialized_ = true;
}

void io_uring_proactor::arm_eventfd() {
    if (event_fd_ < 0) return;
    auto* sqe = ring_.get_sq_entry();
    if (!sqe) return;
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
        ring_.~io_uring_ring();
        std::construct_at(&ring_);
        initialized_ = false;
    }
}

liburingcxx::sq_entry* io_uring_proactor::get_sq_entry() noexcept {
    auto* sqe = ring_.get_sq_entry();
    if (!sqe) {
        log::e("[uring] get_sq_entry() returned NULL — SQ ring full!\n");
    }
    return sqe;
}

std::unique_ptr<io_uring_operation> io_uring_proactor::acquire_operation() {
    auto* sqe = ring_.get_sq_entry();
    if (!sqe) return nullptr;
    return std::make_unique<io_uring_operation>(sqe);
}

int io_uring_proactor::submit(bool wait) noexcept {
    log::v("[uring] submit(wait=%d)\n", wait);
    int ret = ring_.submit_and_wait(wait ? 1 : 0);
    if (ret < 0) {
        if (ret == -EAGAIN || ret == -EINTR) return 0;
        log::e("[uring] submit() failed: %s\n", std::strerror(-ret));
    }
    log::v("[uring] submit returned %d\n", ret);
    return ret;
}

int io_uring_proactor::wait_completion(completion_info* info) noexcept {
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
    if (cqe_ptr->user_data == eventfd_user_data_) {
        log::v("[uring] eventfd CQE — draining cross-thread queue\n");
        ring_.cq_advance(1);
        arm_eventfd(); // re-arm for next wakeup
        return 0;      // caller will drain cross-thread queue
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
    if (event_fd_ >= 0) {
        uint64_t val = 1;
        (void)::write(event_fd_, &val, sizeof(val));
    }
}

intptr_t io_uring_proactor::native_handle() const noexcept {
    return ring_.fd();
}

void io_uring_operation::cancel() noexcept {
    sqe_ = nullptr;
}

int io_uring_proactor::poll_completions_impl(
    void* ctx, void (*callback_fn)(void*, const completion_info*)) noexcept
{
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
