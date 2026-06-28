#pragma once

#include <cstdint>

namespace coronet::config {

enum class log_level : uint8_t { verbose = 0, debug = 1, info = 2, warning = 3, error = 4, no_log = 5 };

// Production
inline constexpr log_level level = log_level::warning;

} // namespace coronet::config
