#pragma once

#include "coronet/platform/platform.hpp"

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#if defined(CORONET_PLATFORM_WINDOWS)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace coronet {

/// 跨平台 IPv4/IPv6 套接字地址 —— 封装 sockaddr_storage，隐藏 AF_INET/AF_INET6 差异。
///
/// Cross-platform IPv4/IPv6 socket address.
///
/// ## 为什么需要 inet_address？
///
/// 原始 POSIX socket API 使用 sockaddr/sockaddr_in/sockaddr_in6 等结构体，
/// 但不同类型之间需要小心转换，且 IPv4 和 IPv6 的处理方式不同。
/// inet_address 在内部使用 sockaddr_storage（足够容纳任意地址族的结构体），
/// 对外提供统一的接口，自动处理 IPv4/IPv6 的差异。
///
/// ## 设计要点
///
///   - 使用 sockaddr_storage 作为底层存储，可同时容纳 IPv4 和 IPv6
///   - ss() / ss6() 访问函数返回对应地址族的类型安全引用
///   - 提供 to_ip(), to_ip_port() 等便于日志和调试的方法
///   - 提供 DNS 解析（resolve / resolve_all）支持主机名到地址的转换
///
/// ## 构造方式
///
/// 1. 默认构造（AF_UNSPEC）：表示无效/未设置地址
/// 2. 字符串 + 端口：自动判断 IPv4 还是 IPv6（inet_pton 探测）
/// 3. 端口号 + IPv6 标志：通配地址（0.0.0.0 或 [::]）
/// 4. 从原生 sockaddr 构造：用于 accept/getpeername 等 API
class inet_address {
public:
    /// Invalid address (AF_UNSPEC)
    inet_address() noexcept { std::memset(&storage_, 0, sizeof(storage_)); ss().sin_family = AF_UNSPEC; }

    /// For connecting: parse ip string + port
    /// 从字符串解析 IP 地址 + 端口用于连接。
    /// 自动尝试 IPv4 和 IPv6，解析失败时设为 AF_UNSPEC。
    inet_address(std::string_view ip, uint16_t port) noexcept;

    /// For listening: bind to all interfaces on port
    /// 构造通配地址（0.0.0.0 或 [::]）用于监听。
    /// @param is_ipv6 true 表示 IPv6 通配地址（in6addr_any）
    explicit inet_address(uint16_t port, bool is_ipv6 = false) noexcept;

    /// From a native sockaddr (returned by accept/getpeername/etc.)
    /// 从 getpeername/accept 返回的原生地址构造。
    explicit inet_address(const struct sockaddr& saddr) noexcept;

    [[nodiscard]] sa_family_t family() const noexcept { return ss().sin_family; }
    [[nodiscard]] uint16_t port() const noexcept { return ntohs(ss().sin_port); }

    /// 修改端口号（用于同一地址不同端口的场景）。
    inet_address& reset_port(uint16_t port) noexcept {
        ss().sin_port = htons(port);
        return *this;
    }

    [[nodiscard]] std::string to_ip() const;
    [[nodiscard]] std::string to_ip_port() const;

    /// 获取指向底层 sockaddr 的指针，用于 bind/connect/accept 等系统调用。
    [[nodiscard]] const struct sockaddr* get_sockaddr() const noexcept {
        return reinterpret_cast<const struct sockaddr*>(&storage_);
    }

    /// 获取 sockaddr 的长度，用于取地址族对应的正确大小。
    [[nodiscard]] socklen_t length() const noexcept {
        return family() == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    }

    bool operator==(const inet_address& rhs) const noexcept;

    /// DNS resolution
    /// Single address resolution (returns first result).
    static bool resolve(std::string_view hostname, uint16_t port, inet_address& out);
    /// Multiple address resolution (returns all results, e.g. round-robin DNS).
    static std::vector<inet_address> resolve_all(
        std::string_view hostname, uint16_t port, const struct addrinfo* hints);

private:
    struct sockaddr_storage storage_;  // 足够容纳任意地址族的存储空间

    // IPv4 类型安全访问
    struct sockaddr_in&  ss()       noexcept { return *reinterpret_cast<struct sockaddr_in*>(&storage_); }
    const struct sockaddr_in&  ss() const noexcept { return *reinterpret_cast<const struct sockaddr_in*>(&storage_); }
    // IPv6 类型安全访问
    struct sockaddr_in6& ss6()       noexcept { return *reinterpret_cast<struct sockaddr_in6*>(&storage_); }
    const struct sockaddr_in6& ss6() const noexcept { return *reinterpret_cast<const struct sockaddr_in6*>(&storage_); }
};

} // namespace coronet
