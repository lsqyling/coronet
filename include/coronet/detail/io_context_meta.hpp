#pragma once

#include <atomic>
#include <cstdint>

namespace coronet::detail {

/// Global io_context registry state.
/// Used for start() barrier synchronisation across all io_contexts.
struct io_context_meta {
    /// Total number of io_contexts created
    std::atomic<uint32_t> create_count{0};

    /// Number of io_contexts that have called start()
    std::atomic<uint32_t> ready_count{0};

    /// Barrier: wait until all created io_contexts have started
    void wait_all_ready() noexcept;
};

/// The global instance
extern io_context_meta g_io_context_meta;

} // namespace coronet::detail
