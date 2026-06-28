#pragma once

#include <type_traits>
#include <memory>

namespace coronet::detail {

/// A storage wrapper that holds space for a T without constructing or destroying it.
template<typename T>
struct uninitialized_buffer {
    alignas(T) std::byte storage[sizeof(T)];

    T* ptr() noexcept { return reinterpret_cast<T*>(&storage); }
    const T* ptr() const noexcept { return reinterpret_cast<const T*>(&storage); }

    T& ref() noexcept { return *ptr(); }
    const T& ref() const noexcept { return *ptr(); }

    template<typename... Args>
    T* construct(Args&&... args) {
        return std::construct_at(ptr(), std::forward<Args>(args)...);
    }

    void destroy() noexcept {
        std::destroy_at(ptr());
    }
};

} // namespace coronet::detail
