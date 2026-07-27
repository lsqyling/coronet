// net_inet_address_gtest.cpp — inet_address unit tests
//
// Tests for coronet::inet_address: IPv4/IPv6 address parsing, formatting,
// comparison, DNS resolution. Pure logic tests — no io_context needed.
//
// Covers:
//   - Default construction (AF_UNSPEC)
//   - IPv4/IPv6 string + port construction (fast success / fast failure)
//   - Wildcard addresses (INADDR_ANY / in6addr_any)
//   - Construction from native sockaddr
//   - to_ip() / to_ip_port() formatting (including P1-10 IPv6 brackets)
//   - Port getter / reset_port
//   - operator== (same family, different family, different address)
//   - length() (IPv4 vs IPv6)
//   - DNS resolve / resolve_all (localhost success, invalid host failure)

#include <gtest/gtest.h>
#include "coronet/net/inet_address.hpp"

#include <string>

using namespace coronet;

// ---- Windows Winsock init guard ----
// inet_pton / inet_ntop / getaddrinfo need WSAStartup on Windows.
// io_context normally does this, but these tests don't use io_context.
#ifdef CORONET_PLATFORM_WINDOWS
#include <winsock2.h>
namespace {
struct WinsockGuard {
    WinsockGuard() { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); }
    ~WinsockGuard() { WSACleanup(); }
};
WinsockGuard g_winsock;
} // namespace
#endif

// ====================================================================
// Construction
// ====================================================================

TEST(InetAddressTest, DefaultConstructor) {
    inet_address addr;
    EXPECT_EQ(addr.family(), AF_UNSPEC);
    EXPECT_EQ(addr.port(), 0);
}

TEST(InetAddressTest, IPv4FromString) {
    inet_address addr("192.168.1.100", 8080);
    EXPECT_EQ(addr.family(), AF_INET);
    EXPECT_EQ(addr.port(), 8080);
    EXPECT_EQ(addr.to_ip(), "192.168.1.100");
}

TEST(InetAddressTest, IPv4Loopback) {
    inet_address addr("127.0.0.1", 443);
    EXPECT_EQ(addr.family(), AF_INET);
    EXPECT_EQ(addr.port(), 443);
    EXPECT_EQ(addr.to_ip(), "127.0.0.1");
}

TEST(InetAddressTest, IPv6FromString) {
    inet_address addr("::1", 9999);
    EXPECT_EQ(addr.family(), AF_INET6);
    EXPECT_EQ(addr.port(), 9999);
    EXPECT_EQ(addr.to_ip(), "::1");
}

TEST(InetAddressTest, IPv6FullAddress) {
    inet_address addr("2001:db8::1", 80);
    EXPECT_EQ(addr.family(), AF_INET6);
    EXPECT_EQ(addr.port(), 80);
}

TEST(InetAddressTest, InvalidIPString) {
    inet_address addr("not.an.ip.address", 80);
    EXPECT_EQ(addr.family(), AF_UNSPEC);
}

TEST(InetAddressTest, EmptyStringIP) {
    inet_address addr("", 80);
    EXPECT_EQ(addr.family(), AF_UNSPEC);
}

// ====================================================================
// Wildcard addresses (for listening)
// ====================================================================

TEST(InetAddressTest, WildcardIPv4) {
    inet_address addr(8080, false);
    EXPECT_EQ(addr.family(), AF_INET);
    EXPECT_EQ(addr.port(), 8080);
    EXPECT_EQ(addr.to_ip(), "0.0.0.0");
}

TEST(InetAddressTest, WildcardIPv6) {
    inet_address addr(9999, true);
    EXPECT_EQ(addr.family(), AF_INET6);
    EXPECT_EQ(addr.port(), 9999);
    EXPECT_EQ(addr.to_ip(), "::");
}

// ====================================================================
// Construction from native sockaddr
// ====================================================================

TEST(InetAddressTest, FromSockaddrIPv4) {
    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(3306);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    inet_address addr(*reinterpret_cast<struct sockaddr*>(&sin));
    EXPECT_EQ(addr.family(), AF_INET);
    EXPECT_EQ(addr.port(), 3306);
    EXPECT_EQ(addr.to_ip(), "127.0.0.1");
}

TEST(InetAddressTest, FromSockaddrIPv6) {
    struct sockaddr_in6 sin6{};
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(5432);
    sin6.sin6_addr = in6addr_loopback;

    inet_address addr(*reinterpret_cast<struct sockaddr*>(&sin6));
    EXPECT_EQ(addr.family(), AF_INET6);
    EXPECT_EQ(addr.port(), 5432);
    EXPECT_EQ(addr.to_ip(), "::1");
}

// ====================================================================
// Formatting: to_ip() and to_ip_port()
// ====================================================================

TEST(InetAddressTest, ToIPv4String) {
    inet_address addr("10.20.30.40", 80);
    EXPECT_EQ(addr.to_ip(), "10.20.30.40");
}

TEST(InetAddressTest, ToIPv6String) {
    inet_address addr("::1", 80);
    EXPECT_EQ(addr.to_ip(), "::1");
}

TEST(InetAddressTest, ToIPPortIPv4) {
    inet_address addr("127.0.0.1", 8080);
    EXPECT_EQ(addr.to_ip_port(), "127.0.0.1:8080");
}

// P1-10 fix: IPv6 addresses must be wrapped in brackets in to_ip_port()
TEST(InetAddressTest, ToIPPortIPv6HasBrackets) {
    inet_address addr("::1", 8080);
    EXPECT_EQ(addr.to_ip_port(), "[::1]:8080");
}

TEST(InetAddressTest, ToIPPortIPv6FullAddress) {
    inet_address addr("2001:db8::1", 443);
    EXPECT_EQ(addr.to_ip_port(), "[2001:db8::1]:443");
}

// ====================================================================
// Port getter and reset_port
// ====================================================================

TEST(InetAddressTest, PortGetter) {
    inet_address addr("1.2.3.4", 12345);
    EXPECT_EQ(addr.port(), 12345);
}

TEST(InetAddressTest, ResetPort) {
    inet_address addr("1.2.3.4", 1000);
    addr.reset_port(2000);
    EXPECT_EQ(addr.port(), 2000);
    // IP should remain unchanged
    EXPECT_EQ(addr.to_ip(), "1.2.3.4");
}

TEST(InetAddressTest, ResetPortIPv6) {
    inet_address addr("::1", 1000);
    addr.reset_port(2000);
    EXPECT_EQ(addr.port(), 2000);
    EXPECT_EQ(addr.family(), AF_INET6);
}

// ====================================================================
// operator==
// ====================================================================

TEST(InetAddressTest, EqualitySame) {
    inet_address a("127.0.0.1", 80);
    inet_address b("127.0.0.1", 80);
    EXPECT_TRUE(a == b);
}

TEST(InetAddressTest, EqualityDifferentPort) {
    inet_address a("127.0.0.1", 80);
    inet_address b("127.0.0.1", 81);
    EXPECT_FALSE(a == b);
}

TEST(InetAddressTest, EqualityDifferentIP) {
    inet_address a("127.0.0.1", 80);
    inet_address b("127.0.0.2", 80);
    EXPECT_FALSE(a == b);
}

TEST(InetAddressTest, EqualityDifferentFamily) {
    inet_address a("127.0.0.1", 80);
    inet_address b("::1", 80);
    EXPECT_FALSE(a == b);
}

TEST(InetAddressTest, EqualityIPv6Same) {
    inet_address a("::1", 80);
    inet_address b("::1", 80);
    EXPECT_TRUE(a == b);
}

// ====================================================================
// length()
// ====================================================================

TEST(InetAddressTest, LengthIPv4) {
    inet_address addr("1.2.3.4", 80);
    EXPECT_EQ(addr.length(), sizeof(struct sockaddr_in));
}

TEST(InetAddressTest, LengthIPv6) {
    inet_address addr("::1", 80);
    EXPECT_EQ(addr.length(), sizeof(struct sockaddr_in6));
}

// ====================================================================
// DNS resolution
// ====================================================================

// Fast success: "localhost" should always resolve
TEST(InetAddressTest, ResolveLocalhost) {
    inet_address out;
    bool ok = inet_address::resolve("localhost", 80, out);
    EXPECT_TRUE(ok);
    // localhost should resolve to 127.0.0.1 or ::1
    EXPECT_TRUE(out.family() == AF_INET || out.family() == AF_INET6);
}

TEST(InetAddressTest, ResolveAllLocalhost) {
    auto results = inet_address::resolve_all("localhost", 80, nullptr);
    EXPECT_FALSE(results.empty());
    for (const auto& addr : results) {
        EXPECT_TRUE(addr.family() == AF_INET || addr.family() == AF_INET6);
    }
}

// Fast failure: .invalid TLD is reserved by RFC 2606 and never resolves
TEST(InetAddressTest, ResolveInvalidHost) {
    inet_address out;
    bool ok = inet_address::resolve("nonexistent.invalid", 80, out);
    EXPECT_FALSE(ok);
}

TEST(InetAddressTest, ResolveAllInvalidHost) {
    auto results = inet_address::resolve_all("nonexistent.invalid", 80, nullptr);
    EXPECT_TRUE(results.empty());
}

// ====================================================================
// get_sockaddr() / length() consistency
// ====================================================================

TEST(InetAddressTest, GetSockaddrNotNull) {
    inet_address addr("127.0.0.1", 80);
    EXPECT_NE(addr.get_sockaddr(), nullptr);
}

TEST(InetAddressTest, GetSockaddrIPv6NotNull) {
    inet_address addr("::1", 80);
    EXPECT_NE(addr.get_sockaddr(), nullptr);
}
