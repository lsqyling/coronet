#pragma once

#include "coronet/config/log.hpp"

#include <cstdio>

namespace coronet {

namespace detail {

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-security"
#endif

template<typename... T>
void log_out(const char* fmt, const T&... args) {
    std::fprintf(stdout, fmt, args...);
    std::fflush(stdout);
}

template<typename... T>
void err_out(const char* fmt, const T&... args) {
    std::fprintf(stderr, fmt, args...);
    std::fflush(stderr);
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

} // namespace detail

namespace log {

template<typename... T>
void v(const char* fmt, const T&... args) {
    if constexpr (config::level <= config::log_level::verbose) {
        detail::log_out(fmt, args...);
    }
}

template<typename... T>
void d(const char* fmt, const T&... args) {
    if constexpr (config::level <= config::log_level::debug) {
        detail::log_out(fmt, args...);
    }
}

template<typename... T>
void i(const char* fmt, const T&... args) {
    if constexpr (config::level <= config::log_level::info) {
        detail::log_out(fmt, args...);
    }
}

template<typename... T>
void w(const char* fmt, const T&... args) {
    if constexpr (config::level <= config::log_level::warning) {
        detail::err_out(fmt, args...);
    }
}

template<typename... T>
void e(const char* fmt, const T&... args) {
    if constexpr (config::level <= config::log_level::error) {
        detail::err_out(fmt, args...);
    }
}

} // namespace log
} // namespace coronet
