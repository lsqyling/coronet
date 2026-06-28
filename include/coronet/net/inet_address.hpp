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

/// Cross-platform IPv4/IPv6 socket address.
class inet_address {
public:
    /// Invalid address (AF_UNSPEC)
    inet_address() noexcept { std::memset(&storage_, 0, sizeof(storage_)); ss().sin_family = AF_UNSPEC; }

    /// For connecting: parse ip string + port
    inet_address(std::string_view ip, uint16_t port) noexcept;

    /// For listening: bind to all interfaces on port
    explicit inet_address(uint16_t port, bool is_ipv6 = false) noexcept;

    /// From a native sockaddr (returned by accept/getpeername/etc.)
    explicit inet_address(const struct sockaddr& saddr) noexcept;

    [[nodiscard]] sa_family_t family() const noexcept { return ss().sin_family; }
    [[nodiscard]] uint16_t port() const noexcept { return ntohs(ss().sin_port); }

    inet_address& reset_port(uint16_t port) noexcept {
        ss().sin_port = htons(port);
        return *this;
    }

    [[nodiscard]] std::string to_ip() const;
    [[nodiscard]] std::string to_ip_port() const;

    [[nodiscard]] const struct sockaddr* get_sockaddr() const noexcept {
        return reinterpret_cast<const struct sockaddr*>(&storage_);
    }

    [[nodiscard]] socklen_t length() const noexcept {
        return family() == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    }

    bool operator==(const inet_address& rhs) const noexcept;

    /// DNS resolution
    static bool resolve(std::string_view hostname, uint16_t port, inet_address& out);
    static std::vector<inet_address> resolve_all(
        std::string_view hostname, uint16_t port, const struct addrinfo* hints);

private:
    struct sockaddr_storage storage_;

    struct sockaddr_in&  ss()       noexcept { return *reinterpret_cast<struct sockaddr_in*>(&storage_); }
    const struct sockaddr_in&  ss() const noexcept { return *reinterpret_cast<const struct sockaddr_in*>(&storage_); }
    struct sockaddr_in6& ss6()       noexcept { return *reinterpret_cast<struct sockaddr_in6*>(&storage_); }
    const struct sockaddr_in6& ss6() const noexcept { return *reinterpret_cast<const struct sockaddr_in6*>(&storage_); }
};

} // namespace coronet
