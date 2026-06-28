#include "coronet/platform/iocp/iocp_proactor.hpp"

#include <cstdio>
#include <cstdlib>

namespace coronet::platform::iocp {

// ---- iocp_operation ----

void iocp_operation::on_pending(iocp_proactor* proactor) {
    if (InterlockedCompareExchange(&ready_, 1, 0) != 0) {
        proactor->post_completion(this, static_cast<DWORD>(InternalHigh), 0);
    }
}

void iocp_operation::on_sync_completion(iocp_proactor* proactor, DWORD bytes) {
    Internal = 0;
    InternalHigh = bytes;
    ready_ = 1;
    proactor->post_completion(this, bytes, 0);
}

// ---- Per-thread operation recycling (ASIO pattern) ----
namespace {

struct op_free_list {
    iocp_operation* head = nullptr;
    int count = 0;
    static constexpr int max_count = 128;
};

thread_local op_free_list tl_ops;

} // anonymous namespace

std::unique_ptr<iocp_operation> iocp_proactor::acquire_operation() {
    auto& fl = tl_ops;
    if (fl.head) {
        auto* op = fl.head;
        fl.head = reinterpret_cast<iocp_operation*>(op->Internal);
        --fl.count;
        op->reset_for_reuse();
        return std::unique_ptr<iocp_operation>{op};
    }
    return std::make_unique<iocp_operation>();
}

void recycle_operation(std::unique_ptr<iocp_operation> op) noexcept {
    if (!op) return;
    auto& fl = tl_ops;
    if (fl.count < op_free_list::max_count) {
        auto* raw = op.release();
        raw->Internal = reinterpret_cast<ULONG_PTR>(fl.head);
        fl.head = raw;
        ++fl.count;
    }
}

// ---- iocp_proactor ----

void iocp_proactor::init(uint32_t entries) {
    entries_ = entries;
    iocp_handle_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (!iocp_handle_) {
        std::fprintf(stderr, "iocp_proactor: CreateIoCompletionPort failed\n");
        std::abort();
    }
}

void iocp_proactor::deinit() noexcept {
    if (iocp_handle_) {
        PostQueuedCompletionStatus(iocp_handle_, 0, 1, nullptr);
    }
    for (int i = 0; i < 1000; ++i) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* ov = nullptr;
        BOOL ok = GetQueuedCompletionStatus(iocp_handle_, &bytes, &key, &ov, 0);
        if (!ok && !ov) break;
        if (key == 1 && !ov) continue;
        if (ov) {
            std::unique_ptr<iocp_operation> op{iocp_operation::from_overlapped(ov)};
        }
    }
    if (iocp_handle_) {
        CloseHandle(iocp_handle_);
        iocp_handle_ = nullptr;
    }
    outstanding_work_.store(0, std::memory_order_release);
}

int iocp_proactor::submit(bool /*wait*/) noexcept {
    return 0;
}

int iocp_proactor::wait_completion(completion_info* info) noexcept {
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* overlapped = nullptr;
    BOOL ok = GetQueuedCompletionStatus(
        iocp_handle_, &bytes, &key, &overlapped, INFINITE);
    if (!overlapped) {
        if (key == 1) return 0;
        return 0;
    }
    auto* op = iocp_operation::from_overlapped(overlapped);
    if (op) {
        if (!op->is_ready()) return 0;
        info->user_data = op->get_user_data();
        info->opaque = op;
    } else {
        info->user_data = static_cast<uint64_t>(key);
        info->opaque = nullptr;
    }
    info->result = ok ? static_cast<int32_t>(bytes)
                      : -static_cast<int32_t>(::GetLastError());
    info->flags = ok ? 0 : 1;
    return 1;
}

intptr_t iocp_proactor::native_handle() const noexcept {
    return reinterpret_cast<intptr_t>(iocp_handle_);
}

void iocp_proactor::wakeup() noexcept {
    if (iocp_handle_) {
        PostQueuedCompletionStatus(iocp_handle_, 0, 1, nullptr);
    }
}

void iocp_proactor::post_completion(iocp_operation* op, DWORD bytes, DWORD key) {
    PostQueuedCompletionStatus(
        reinterpret_cast<HANDLE>(iocp_handle_), bytes, key,
        static_cast<OVERLAPPED*>(op));
}

void iocp_proactor::register_handle(uintptr_t sock) noexcept {
    CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(sock),
        reinterpret_cast<HANDLE>(iocp_handle_), 0, 0);
}

int iocp_proactor::poll_completions_impl(
    void* ctx, void (*callback_fn)(void*, const completion_info*)) noexcept
{
    completion_info info{};
    int ret = wait_completion(&info);
    if (ret > 0) callback_fn(ctx, &info);
    return ret;
}

} // namespace coronet::platform::iocp
