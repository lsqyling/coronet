#pragma once

#include <type_traits>

namespace coronet::detail {

/// Concept for "task-like" types (have a promise_type)
template<typename T>
concept tasklike = requires {
    typename T::promise_type;
    typename T::value_type;
    typename T::is_task_like;
};

} // namespace coronet::detail
