#include "coronet/net/inet_address.hpp"

#include <cstring>

namespace coronet {

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

inet_address::inet_address(const struct sockaddr& saddr) noexcept {
    std::memset(&storage_, 0, sizeof(storage_));
    if (saddr.sa_family == AF_INET6) {
        std::memcpy(&ss6(), &saddr, sizeof(ss6()));
    } else {
        std::memcpy(&ss(), &saddr, sizeof(ss()));
    }
}

std::string inet_address::to_ip() const {
    char buf[INET6_ADDRSTRLEN] = {};
    if (family() == AF_INET6) {
        ::inet_ntop(AF_INET6, &ss6().sin6_addr, buf, sizeof(buf));
    } else {
        ::inet_ntop(AF_INET, &ss().sin_addr, buf, sizeof(buf));
    }
    return std::string(buf);
}

std::string inet_address::to_ip_port() const {
    return to_ip() + ":" + std::to_string(port());
}

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

bool inet_address::resolve(std::string_view hostname, uint16_t port,
                           inet_address& out) {
    auto results = resolve_all(hostname, port, nullptr);
    if (results.empty()) return false;
    out = results.front();
    return true;
}

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
