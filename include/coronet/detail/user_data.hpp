#pragma once
#include <cstdint>

namespace coronet::detail {

/// Reserved user_data values for internal operations.
enum class reserved_user_data : uint64_t {
    nop,
    none
};

/// Type tag stored in bits [2:0] of user_data.
enum class user_data_type : uint8_t {
    task_info_ptr,
    none
};

static_assert(uint8_t(user_data_type::none) <= 8);

} // namespace coronet::detail
