// ============================================================
// epoll_reactor.cpp — epoll Proactor 实现
// ============================================================
// 核心流程：
//   1. init()     → epoll_create1 + eventfd + arm_eventfd
//   2. await_suspend 时 awaiter 调用 register_fd() 将 fd 注册到 epoll
//   3. 事件循环中 wait_completion() → fill_ready_queue(epoll_wait) → 取出事件
//      → 调用 ctx->perform(fd, self) 执行实际 I/O → 填充 completion_info
//   4. 跨线程唤醒：wakeup() 往 eventfd 写数据 → epoll 返回 eventfd 事件
//      → drain_eventfd + arm_eventfd → 事件循环 drain_cross_thread

#include "coronet/platform/epoll/epoll_reactor.hpp"
#include "coronet/log/log.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/eventfd.h>
#include <unistd.h>

namespace coronet::platform::epoll {

// ============================================================
// 初始化 / 清理
// ============================================================

void epoll_proactor::init(uint32_t max_events) {
    if (initialized_) return;
    log::d("[epoll] init(max_events=%u)\n", max_events);
    max_events_ = max_events;

    // 创建 epoll 实例 / Create epoll instance
    epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0) {
        log::e("[epoll] epoll_create1 failed: %s\n", std::strerror(errno));
        std::abort();
    }
    log::d("[epoll] epfd=%d\n", epfd_);

    // 预分配就绪事件缓冲区 / Pre-allocate the ready events buffer
    ready_events_.resize(max_events_);
    ready_pos_ = 0;
    ready_count_ = 0;

    // 创建 eventfd 用于跨线程唤醒 / Create eventfd for cross-thread wakeup
    eventfd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (eventfd_ < 0) {
        log::e("[epoll] eventfd creation failed: %s\n", std::strerror(errno));
        ::close(epfd_);
        epfd_ = -1;
        std::abort();
    }
    // 将 eventfd 注册到 epoll（首次唤醒用）
    arm_eventfd();
    log::d("[epoll] eventfd=%d armed\n", eventfd_);

    initialized_ = true;
}

void epoll_proactor::deinit() noexcept {
    if (initialized_) {
        if (eventfd_ >= 0) {
            ::close(eventfd_);
            eventfd_ = -1;
        }
        if (epfd_ >= 0) {
            ::close(epfd_);
            epfd_ = -1;
        }
        ready_events_.clear();
        ready_pos_ = 0;
        ready_count_ = 0;
        initialized_ = false;
    }
}

// ============================================================
// eventfd 管理（跨线程唤醒）
// ============================================================

void epoll_proactor::arm_eventfd() {
    if (eventfd_ < 0 || epfd_ < 0) return;
    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;      // 边沿触发，避免重复唤醒
    ev.data.fd = eventfd_;              // 用 data.fd 标识唤醒事件
    // 先尝试 ADD；若已注册（EEXIST）则用 MOD 重新 arming
    // Try ADD first; if already registered (EEXIST), use MOD for re-arm.
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, eventfd_, &ev) < 0) {
        if (errno == EEXIST) {
            ::epoll_ctl(epfd_, EPOLL_CTL_MOD, eventfd_, &ev);
        } else {
            log::w("[epoll] arm_eventfd epoll_ctl failed: %s\n", std::strerror(errno));
        }
    }
}

void epoll_proactor::drain_eventfd() noexcept {
    if (eventfd_ < 0) return;
    uint64_t val = 0;
    // 读取 eventfd 以清除可读状态（边沿触发需要）
    ssize_t n = ::read(eventfd_, &val, sizeof(val));
    (void)n;
}

// ============================================================
// 就绪队列管理
// ============================================================

bool epoll_proactor::fill_ready_queue(bool wait) noexcept {
    if (epfd_ < 0) return false;
    // wait=true → 无限等待；wait=false → 立即返回
    int timeout_ms = wait ? -1 : 0;
    int nfds = ::epoll_wait(epfd_, ready_events_.data(),
                            static_cast<int>(max_events_), timeout_ms);
    if (nfds < 0) {
        if (errno == EINTR) {
            // 被信号中断，返回空队列 / Interrupted by signal, return empty
            ready_count_ = 0;
            ready_pos_ = 0;
            return false;
        }
        log::e("[epoll] epoll_wait failed: %s\n", std::strerror(errno));
        ready_count_ = 0;
        ready_pos_ = 0;
        return false;
    }
    ready_count_ = static_cast<size_t>(nfds);
    ready_pos_ = 0;
    log::v("[epoll] epoll_wait returned %d events\n", nfds);
    return nfds > 0;
}

// ============================================================
// Proactor 标准接口
// ============================================================

int epoll_proactor::submit(bool wait) noexcept {
    log::v("[epoll] submit(wait=%d)\n", wait);
    // epoll 的 submit 语义：调用 epoll_wait 预取事件到内部队列
    if (fill_ready_queue(wait)) {
        return static_cast<int>(ready_count_);
    }
    return 0;
}

int epoll_proactor::wait_completion(completion_info* info) noexcept {
    // 内部队列为空 → 阻塞等待新事件 / If no ready events, block until one arrives
    if (ready_pos_ >= ready_count_) {
        if (!fill_ready_queue(true)) {
            return 0;
        }
    }
    if (ready_pos_ >= ready_count_) {
        return 0;
    }

    // 从就绪队列取出一个事件 / Pop one event from the ready queue
    struct epoll_event& ev = ready_events_[ready_pos_++];
    log::v("[epoll] wait_completion: fd=%d events=0x%x\n",
           ev.data.fd, ev.events);

    // 检查是否为 eventfd 唤醒事件（跨线程 co_spawn）
    // Check if this is the eventfd (cross-thread wakeup)
    if (ev.data.fd == eventfd_) {
        log::v("[epoll] eventfd triggered — draining\n");
        drain_eventfd();          // 清除 eventfd 可读状态
        arm_eventfd();            // 重新 arming 以备下次唤醒
        return 0;                 // 通知调用者排空跨线程队列
    }

    // 从 epoll_event 中提取完成上下文 / Get the completion context
    auto* ctx = static_cast<epoll_completion_ctx*>(ev.data.ptr);
    if (!ctx || !ctx->perform) {
        log::w("[epoll] wait_completion: null completion ctx or perform fn\n");
        return 0;
    }

    // 执行实际的 I/O syscall（用 ctx->fd 而非 ev.data.fd，因为 data.ptr 覆盖了 data.fd）
    // Perform the actual I/O syscall (use ctx->fd because data.fd is clobbered by data.ptr)
    int fd = ctx->fd;
    int result = ctx->perform(fd, ctx->self);
    log::v("[epoll] perform(fd=%d) returned %d\n", fd, result);

    // 填充跨平台的 completion_info / Populate completion_info
    info->user_data = ctx->user_data;
    info->result = result;
    info->flags = ev.events;
    info->opaque = nullptr;

    return 1;
}

void epoll_proactor::wakeup() noexcept {
    // 往 eventfd 写入 1 字节 → epoll_wait 返回 eventfd 可读 → 解除阻塞
    if (eventfd_ >= 0) {
        uint64_t val = 1;
        ssize_t n = ::write(eventfd_, &val, sizeof(val));
        (void)n;
    }
}

std::unique_ptr<epoll_operation> epoll_proactor::acquire_operation() {
    // epoll 中几乎不用此接口（操作由 epoll_completion_ctx 承载）
    return std::make_unique<epoll_operation>();
}

// ============================================================
// Epoll fd 注册管理
// ============================================================

void epoll_proactor::register_fd(int fd, uint32_t events,
                                  epoll_completion_ctx* ctx) noexcept {
    if (epfd_ < 0 || fd < 0) return;
    struct epoll_event ev{};
    // EPOLLONESHOT：触发一次后自动移除，避免惊群
    // EPOLLET：     边沿触发，只在状态变化时通知一次
    ev.events = events | EPOLLONESHOT | EPOLLET;
    ev.data.ptr = ctx;   // 存储完成上下文指针
    log::v("[epoll] register_fd: fd=%d events=0x%x ctx=%p\n", fd, ev.events, (void*)ctx);

    // 先尝试 ADD；如果 fd 已注册（上一操作尚未触发 EPOLLONESHOT），用 MOD
    // Try ADD first; if already registered, use MOD (e.g. EPOLLONESHOT not yet fired).
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        if (errno == EEXIST) {
            if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
                log::e("[epoll] register_fd epoll_ctl(MOD) fd=%d failed: %s\n",
                       fd, std::strerror(errno));
            }
        } else {
            log::e("[epoll] register_fd epoll_ctl(ADD) fd=%d failed: %s\n",
                   fd, std::strerror(errno));
        }
    }
}

void epoll_proactor::modify_fd(int fd, uint32_t events,
                                epoll_completion_ctx* ctx) noexcept {
    if (epfd_ < 0 || fd < 0) return;
    struct epoll_event ev{};
    ev.events = events | EPOLLONESHOT | EPOLLET;
    ev.data.ptr = ctx;
    if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        log::w("[epoll] modify_fd epoll_ctl(MOD) fd=%d failed: %s\n",
               fd, std::strerror(errno));
    }
}

void epoll_proactor::unregister_fd(int fd) noexcept {
    if (epfd_ < 0 || fd < 0) return;
    ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
}

// ============================================================
// 批量完成收割（用于批量处理场景）
// ============================================================

int epoll_proactor::poll_completions_impl(
    void* ctx, void (*callback_fn)(void*, const completion_info*)) noexcept
{
    int count = 0;
    // 先处理已就绪但未消费的事件 / Process any already-ready events
    while (ready_pos_ < ready_count_) {
        struct epoll_event& ev = ready_events_[ready_pos_++];
        if (ev.data.fd == eventfd_) {
            drain_eventfd();
            arm_eventfd();
            continue;
        }
        auto* ep_ctx = static_cast<epoll_completion_ctx*>(ev.data.ptr);
        if (!ep_ctx || !ep_ctx->perform) continue;
        int result = ep_ctx->perform(ep_ctx->fd, ep_ctx->self);
        completion_info info{};
        info.user_data = ep_ctx->user_data;
        info.result = result;
        info.flags = ev.events;
        info.opaque = nullptr;
        callback_fn(ctx, &info);
        ++count;
    }
    // 再非阻塞地轮询新事件 / Also poll for new events without blocking
    if (fill_ready_queue(false)) {
        while (ready_pos_ < ready_count_) {
            struct epoll_event& ev = ready_events_[ready_pos_++];
            if (ev.data.fd == eventfd_) {
                drain_eventfd();
                arm_eventfd();
                continue;
            }
            auto* ep_ctx = static_cast<epoll_completion_ctx*>(ev.data.ptr);
            if (!ep_ctx || !ep_ctx->perform) continue;
            int result = ep_ctx->perform(ep_ctx->fd, ep_ctx->self);
            completion_info info{};
            info.user_data = ep_ctx->user_data;
            info.result = result;
            info.flags = ev.events;
            info.opaque = nullptr;
            callback_fn(ctx, &info);
            ++count;
        }
    }
    return count;
}

} // namespace coronet::platform::epoll
