#pragma once

#include "coronet/platform/platform.hpp"

#include <concepts>
#include <cstdint>
#include <type_traits>

namespace coronet::platform {

// ============================================================
// C++20 Concepts — replace virtual base classes with static polymorphism
// ============================================================

/// Concept for a platform I/O operation.
/// Satisfied by iocp_proactor::iocp_operation and io_uring_operation.
template<typename T>
concept operation_concept = requires(T op, uint64_t ud) {
    { op.set_user_data(ud) } -> std::same_as<void>;
    { op.prepare() } -> std::same_as<void>;
    { op.cancel() } -> std::same_as<void>;
};

/// Concept for a platform Proactor (IOCP or io_uring).
/// io_context holds a concrete proactor satisfying this concept.
template<typename T>
concept proactor_concept = requires(T p, uint32_t entries, completion_info* info) {
    { p.init(entries) } -> std::same_as<void>;
    { p.deinit() } -> std::same_as<void>;
    { p.submit(false) } -> std::same_as<int>;
    { p.wait_completion(info) } -> std::same_as<int>;
    { p.native_handle() } -> std::same_as<intptr_t>;
    { p.wakeup() } -> std::same_as<void>;
    typename T::operation_type;
};

// ============================================================
// poll_completions: template helper (works with any proactor)
// ============================================================

template<typename Proactor, typename F>
    requires proactor_concept<Proactor>
int poll_completions(Proactor& p, F&& callback) noexcept {
    struct call_ctx { F* cb; };
    call_ctx ctx{&callback};
    auto thunk = [](void* vctx, const completion_info* info) noexcept {
        auto* c = static_cast<call_ctx*>(vctx);
        (*c->cb)(info);
    };
    return p.poll_completions_impl(&ctx, thunk);
}

} // namespace coronet::platform
