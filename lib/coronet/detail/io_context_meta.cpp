#include "coronet/detail/io_context_meta.hpp"

#include <thread>

namespace coronet::detail {

io_context_meta g_io_context_meta;

void io_context_meta::wait_all_ready() noexcept {
    // Spin-wait until all created io_contexts have called start()
    // In a proper implementation this would use a condition_variable,
    // but a simple spin is acceptable for the common case (1-4 contexts).
    while (ready_count.load(std::memory_order_acquire) <
           create_count.load(std::memory_order_acquire)) {
        // yield
        std::this_thread::yield();
    }
}

} // namespace coronet::detail
