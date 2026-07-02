#pragma once

#include "coronet/config/log.hpp"

#include <cstdio>

namespace coronet {

namespace detail {

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-security"
#endif

/*
 * 日志输出的底层函数。
 * 使用 std::fprintf + std::fflush 而非 std::cout / std::format，
 * 原因如下：
 * - fprintf 是 C 标准库函数，没有 iostream 的虚函数调用和同步开销
 * - fflush 确保日志立即看到，不会因缓冲而丢失崩溃前的日志
 * - 避免引入 <iostream> 头文件（它会增加二进制体积和启动延迟）
 * - 日志是性能敏感路径（尤其是 verbose 级别），直接使用最轻量的输出方式
 *
 * 压栈 GCC 的 -Wformat-security 警告是因为我们允许用户传递运行时
 * 格式字符串（这在严格的安全审计中不被推荐，但对调试日志可以接受）。
 */
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

/*
 * 日志门面函数：v（verbose）、d（debug）、i（info）、w（warning）、e（error）。
 *
 * 为什么是模板函数而非 printf 风格的宏：
 * - 使用 if constexpr 在编译期判断日志级别，当日志被禁用时，
 *   编译器不会生成任何调用代码和字符串字面量，实现零开销。
 * - 相比宏，模板函数有更好的类型安全和作用域控制。
 * - 编译器完全优化掉不可达分支（dead code elimination）。
 *
 * 注意：当日志级别高于调用级别时，参数表达式不会被求值，
 * 但参数类型仍会被推导。如果参数构造有副作用，需要注意。
 */
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

/*
 * warning 和 error 级别的日志输出到 stderr 而非 stdout：
 * - stderr 通常是无缓冲的（或行缓冲），错误消息能立即显示
 * - stdout 被重定向时，stderr 仍然会在终端显示，便于区分正常输出和错误
 * - 符合 UNIX 惯例：正常输出走 stdout，错误诊断走 stderr
 */
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
