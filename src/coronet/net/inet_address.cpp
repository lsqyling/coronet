#include "coronet/net/inet_address.hpp"

#include <cstring>

namespace coronet {

/// 从 IP 字符串 + 端口构造地址。
///
/// 先尝试 IPv4（inet_pton AF_INET），如果失败再尝试 IPv6。
/// 这种"先 IPv4 再 IPv6"的顺序符合大多数场景的预期 ——
/// 在同机测试和大多数局域网环境中 IPv4 更常用。
///
/// 如果两种协议都解析失败，将 family 设为 AF_UNSPEC 表示无效地址。
inet_address::inet_address(std::string_view ip, uint16_t port) noexcept {
    std::memset(&storage_, 0, sizeof(storage_));

    // Try IPv4 first
    ss().sin_family = AF_INET;
    ss().sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.data(), &ss().sin_addr) == 1) {
        return; // IPv4 success
    }

    // Try IPv6
    ss6().sin6_family = AF_INET6;
    ss6().sin6_port = htons(port);
    if (::inet_pton(AF_INET6, ip.data(), &ss6().sin6_addr) == 1) {
        return; // IPv6 success
    }

    // Fallback: set AF_UNSPEC to indicate failure
    ss().sin_family = AF_UNSPEC;
}

/// 构造通配地址（用于监听）。
///
/// IPv4：0.0.0.0（INADDR_ANY），监听所有网络接口的 IPv4 连接。
/// IPv6：[::]（in6addr_any），监听所有网络接口的 IPv6 连接。
///
/// @param is_ipv6 设为 true 使用 IPv6 通配地址
inet_address::inet_address(uint16_t port, bool is_ipv6) noexcept {
    std::memset(&storage_, 0, sizeof(storage_));
    if (is_ipv6) {
        ss6().sin6_family = AF_INET6;
        ss6().sin6_port = htons(port);
        ss6().sin6_addr = in6addr_any;
    } else {
        ss().sin_family = AF_INET;
        ss().sin_port = htons(port);
        ss().sin_addr.s_addr = INADDR_ANY;
    }
}

/// 从原生 sockaddr 构造（用于 getpeername / accept 等系统调用的返回值）。
///
/// 根据 sa_family 自动判断是 IPv4 还是 IPv6 并做对应大小的拷贝。
inet_address::inet_address(const struct sockaddr& saddr) noexcept {
    std::memset(&storage_, 0, sizeof(storage_));
    if (saddr.sa_family == AF_INET6) {
        std::memcpy(&ss6(), &saddr, sizeof(ss6()));
    } else {
        std::memcpy(&ss(), &saddr, sizeof(ss()));
    }
}

/// 将 IP 地址转换为字符串表示。
///
/// 使用 inet_ntop（线程安全，支持 IPv4/IPv6）。
/// 缓冲区大小设为 INET6_ADDRSTRLEN（46 字节），足以容纳 IPv6 地址。
std::string inet_address::to_ip() const {
    char buf[INET6_ADDRSTRLEN] = {};
    if (family() == AF_INET6) {
        ::inet_ntop(AF_INET6, &ss6().sin6_addr, buf, sizeof(buf));
    } else {
        ::inet_ntop(AF_INET, &ss().sin_addr, buf, sizeof(buf));
    }
    return std::string(buf);
}

/// 格式化为 "IP:端口" 字符串，便于日志和调试。
std::string inet_address::to_ip_port() const {
    return to_ip() + ":" + std::to_string(port());
}

/// 比较两个地址是否相等（族、端口、地址均相同）。
bool inet_address::operator==(const inet_address& rhs) const noexcept {
    if (family() != rhs.family()) return false;
    if (port() != rhs.port()) return false;
    if (family() == AF_INET6) {
        return std::memcmp(&ss6().sin6_addr, &rhs.ss6().sin6_addr,
                           sizeof(ss6().sin6_addr)) == 0;
    }
    return std::memcmp(&ss().sin_addr, &rhs.ss().sin_addr,
                       sizeof(ss().sin_addr)) == 0;
}

/// DNS 解析（单结果）。
///
/// 使用 getaddrinfo 执行 DNS 解析，返回第一个可用的地址。
/// 适用于只需要一个连接地址的场景。
/// 对于有多个 A/AAAA 记录的域名，仅返回解析到的第一个地址。
///
/// @param hostname 主机名（如 "example.com"）
/// @param port 端口号
/// @param out 输出参数，解析到的地址
/// @return 解析成功返回 true
bool inet_address::resolve(std::string_view hostname, uint16_t port,
                           inet_address& out) {
    auto results = resolve_all(hostname, port, nullptr);
    if (results.empty()) return false;
    out = results.front();
    return true;
}

/// DNS 解析（多结果）。
///
/// 返回所有解析到的地址。适用于需要尝试多个地址的场景
/// （例如轮询 DNS 或同时支持 IPv4/IPv6）。
///
/// @param hostname 主机名
/// @param port 端口号
/// @param hints 可选参数，用于指定地址族等过滤条件（如 ai_family = AF_INET6）
/// @return 解析到的地址列表（可能为空）
std::vector<inet_address> inet_address::resolve_all(
    std::string_view hostname, uint16_t port, const struct addrinfo* hints)
{
    std::vector<inet_address> result;
    std::string port_str = std::to_string(port);

    struct addrinfo* ai_list = nullptr;
    int rc = ::getaddrinfo(hostname.data(), port_str.c_str(), hints, &ai_list);
    if (rc != 0) return result;

    for (struct addrinfo* ai = ai_list; ai != nullptr; ai = ai->ai_next) {
        if (ai->ai_addr) {
            result.emplace_back(*ai->ai_addr);
        }
    }

    ::freeaddrinfo(ai_list);
    return result;
}

} // namespace coronet
