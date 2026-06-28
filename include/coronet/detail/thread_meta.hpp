#pragma once

#include "coronet/config/io_context.hpp"
#include <atomic>

namespace coronet { class io_context; }
namespace coronet::detail { struct worker_meta; }

namespace coronet::detail {

/// Per-thread metadata: gives O(1) access to the current io_context and worker.
struct alignas(config::cache_line_size) thread_meta {
    /// The io_context this thread is running
    io_context* ctx = nullptr;

    /// The worker_meta (cache-line-aligned member of io_context)
    worker_meta* worker = nullptr;

    /// io_context id (unique among all io_contexts)
    config::ctx_id_t ctx_id = config::ctx_id_t(-1);
};

/// Thread-local current context
extern thread_local thread_meta this_thread;

} // namespace coronet::detail
