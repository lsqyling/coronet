#include "coronet/detail/worker_meta.hpp"
#include "coronet/detail/io_context_meta.hpp"
#include "coronet/detail/thread_meta.hpp"
#include "coronet/log/log.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>

namespace coronet::detail {

void worker_meta::init(uint32_t entries) {
    assert(proactor);
    proactor->init(entries);
}

void worker_meta::deinit() noexcept {
    if (proactor) proactor->deinit();
}

void worker_meta::co_spawn_unsafe(std::coroutine_handle<> handle) noexcept {
    forward_task(handle);
}

void worker_meta::co_spawn_auto(std::coroutine_handle<> handle) noexcept {
    // If called from another thread, use the thread-safe cross-thread path
    if (detail::this_thread.worker != this && detail::g_io_context_meta.ready_count > 0) {
        co_spawn_cross(handle);
        return;
    }
    forward_task(handle);
}

void worker_meta::co_spawn_cross(std::coroutine_handle<> handle) noexcept {
    bool need_wakeup = false;
    {
        std::lock_guard lock(cross_mtx);
        need_wakeup = cross_queue.empty();
        if (cross_queue.capacity() < 64) cross_queue.reserve(64);
        cross_queue.push_back(handle);
    }
    // Only wake the worker if queue was previously empty.
    // Eliminates redundant PostQueuedCompletionStatus calls at high concurrency.
    if (need_wakeup && proactor) {
        proactor->wakeup();
    }
}

void worker_meta::drain_cross_thread() noexcept {
    if (cross_queue.empty()) return;

    thread_local std::vector<std::coroutine_handle<>> batch;
    {
        std::lock_guard lock(cross_mtx);
        batch.swap(cross_queue);
    }
    for (auto h : batch) {
        forward_task(h);
    }
    batch.clear();
}

std::coroutine_handle<> worker_meta::schedule() noexcept {
    config::cur_t slot = reap_cur.pop();
    if (slot == config::cur_t(-1)) [[unlikely]]
        return nullptr;
    return reap_swap[slot];
}

void worker_meta::forward_task(std::coroutine_handle<> handle) noexcept {
    config::cur_t slot = reap_cur.push();
    if (slot == config::cur_t(-1)) [[unlikely]] {
        std::fprintf(stderr, "worker_meta: reap_swap overflow!\n");
        std::abort();
    }
    reap_swap[slot] = handle;
}

void worker_meta::work_once() {
    auto handle = schedule();
    if (handle) handle.resume();
}

void worker_meta::poll_submission() noexcept {
    if (requests_to_submit == 0) [[likely]]
        return;
    int ret = proactor->submit(false);
    if (ret < 0) {
        log::e("[worker] poll_submission failed: %d\n", ret);
    }
    requests_to_submit = 0;
}

uint32_t worker_meta::poll_completion() noexcept {
    platform::completion_info info{};
    int ret = proactor->wait_completion(&info);
    if (ret > 0) {
        handle_completion(&info);
        return 1;
    }
    return 0;
}

void worker_meta::handle_completion(
    const platform::completion_info* info) noexcept
{
    --requests_to_reap;
    log::v("[worker] handle_completion: user_data=%llu result=%d reap=%u\n",
           (unsigned long long)info->user_data, info->result, requests_to_reap);

    auto* ti = task_info::from_user_data(info->user_data);
    if (!ti) {
        log::w("[worker] handle_completion: null task_info for user_data\n");
        return;
    }

    ti->result = info->result;

    // Chained co_await: first op completed → auto-start the second op
    if (ti->chain_fn && ti->chain_ctx) {
        auto fn = ti->chain_fn;
        auto* ctx = ti->chain_ctx;
        ti->chain_fn = nullptr;
        ti->chain_ctx = nullptr;
        fn(ctx);  // start second I/O (its handle leads to user coroutine)
        return;
    }

    if (!ti->handle) {
        // Expected for linked SQEs (first op in chain has null handle)
        log::v("[worker] handle_completion: null handle (linked SQE or chain)\n");
        return;
    }

    log::v("[worker] forwarding task handle=%p\n", ti->handle.address());
    forward_task(ti->handle);

    ti->handle = nullptr;

    // Recycle the platform operation via unique_ptr (no raw delete)
#if defined(CORONET_PLATFORM_WINDOWS)
    if (info->opaque) {
        auto* raw = static_cast<platform::iocp::iocp_operation*>(info->opaque);
        platform::iocp::recycle_operation(std::unique_ptr<platform::iocp::iocp_operation>{raw});
    }
#else
    (void)info->opaque;
#endif
}

void worker_meta::check_submission_threshold() noexcept {
    if (requests_to_submit >= config::submission_threshold) {
        poll_submission();
    }
}

} // namespace coronet::detail
