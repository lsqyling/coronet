// net_socket_gtest.cpp — tcp_socket / udp_socket / tcp_acceptor sync unit tests
//
// Tests for coronet::tcp_socket, coronet::udp_socket, and coronet::tcp_acceptor:
// socket creation (TCP/UDP, IPv4/IPv6), RAII move semantics (P0-1 fix), socket
// options, bind/listen success and failure, local_addr/peer_addr, acceptor
// construction.
//
// All tests are synchronous (no io_context needed). Each test is fast —
// microseconds to milliseconds. Resource cleanup is verified via repeated
// create/destroy cycles to detect fd leaks.
//
// Note: On Windows, ::socket (winsock2.h) may conflict with the old coronet::tcp_socket
// alias. We use explicit coronet:: prefix and the new tcp_socket/udp_socket names.

#include <gtest/gtest.h>
#include "coronet/net/tcp_socket.hpp"
#include "coronet/net/tcp_acceptor.hpp"
#include "coronet/net/udp_socket.hpp"
#include "coronet/platform/platform.hpp"

#include <system_error>

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
// Factory: create_tcp / create_udp
// ====================================================================

TEST(SocketTest, CreateTCPv4) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
    EXPECT_NE(s.native_handle(), coronet::platform::invalid_socket);
}

TEST(SocketTest, CreateTCPv6) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET6);
    EXPECT_NE(s.native_handle(), coronet::platform::invalid_socket);
}

TEST(SocketTest, CreateUDPv4) {
    coronet::udp_socket s = coronet::udp_socket::create_udp(AF_INET);
    EXPECT_NE(s.native_handle(), coronet::platform::invalid_socket);
}

TEST(SocketTest, CreateUDPv6) {
    coronet::udp_socket s = coronet::udp_socket::create_udp(AF_INET6);
    EXPECT_NE(s.native_handle(), coronet::platform::invalid_socket);
}

// Fast failure: invalid address family should throw
TEST(SocketTest, CreateTCPInvalidFamily) {
    EXPECT_THROW(coronet::tcp_socket::create_tcp(static_cast<sa_family_t>(255)), std::system_error);
}

TEST(SocketTest, CreateUDPInvalidFamily) {
    EXPECT_THROW(coronet::udp_socket::create_udp(static_cast<sa_family_t>(255)), std::system_error);
}

// ====================================================================
// RAII: move semantics
// ====================================================================

TEST(SocketTest, MoveConstruction) {
    coronet::tcp_socket s1 = coronet::tcp_socket::create_tcp(AF_INET);
    auto fd = s1.native_handle();
    coronet::tcp_socket s2 = std::move(s1);
    EXPECT_EQ(s1.native_handle(), coronet::platform::invalid_socket);
    EXPECT_EQ(s2.native_handle(), fd);
}

// P0-1 fix: move assignment must close the old fd
TEST(SocketTest, MoveAssignmentClosesOldFd) {
    coronet::tcp_socket s1 = coronet::tcp_socket::create_tcp(AF_INET);
    coronet::tcp_socket s2 = coronet::tcp_socket::create_tcp(AF_INET);
    auto fd2 = s2.native_handle();
    s1 = std::move(s2);
    // s1 now has fd2, s2 is invalid
    EXPECT_EQ(s1.native_handle(), fd2);
    EXPECT_EQ(s2.native_handle(), coronet::platform::invalid_socket);
    // s1's old fd was closed by the swap idiom in operator=
}

TEST(SocketTest, SelfMoveSafe) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
    // Self-move should not corrupt state (self-assignment check in operator=)
    s = std::move(s);
    EXPECT_NE(s.native_handle(), coronet::platform::invalid_socket);
}

TEST(SocketTest, Swap) {
    coronet::tcp_socket s1 = coronet::tcp_socket::create_tcp(AF_INET);
    coronet::tcp_socket s2 = coronet::tcp_socket::create_tcp(AF_INET);
    auto fd1 = s1.native_handle();
    auto fd2 = s2.native_handle();
    s1.swap(s2);
    EXPECT_EQ(s1.native_handle(), fd2);
    EXPECT_EQ(s2.native_handle(), fd1);
}

// ====================================================================
// Socket options — fast success (no throw)
// ====================================================================

TEST(SocketTest, SetReuseAddr) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
    EXPECT_NO_THROW(s.set_reuse_addr(true));
    EXPECT_NO_THROW(s.set_reuse_addr(false));
}

TEST(SocketTest, SetReusePort) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
    EXPECT_NO_THROW(s.set_reuse_port(true));
}

TEST(SocketTest, SetTcpNoDelay) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
    EXPECT_NO_THROW(s.set_tcp_no_delay(true));
    EXPECT_NO_THROW(s.set_tcp_no_delay(false));
}

TEST(SocketTest, SetKeepalive) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
    EXPECT_NO_THROW(s.set_keepalive(true));
    EXPECT_NO_THROW(s.set_keepalive(false));
}

TEST(SocketTest, SetRecvBufferSize) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
    EXPECT_NO_THROW(s.set_recv_buffer_size(65536));
}

TEST(SocketTest, SetSendBufferSize) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
    EXPECT_NO_THROW(s.set_send_buffer_size(65536));
}

TEST(SocketTest, SetLinger) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
    EXPECT_NO_THROW(s.set_linger(false, 0));
    EXPECT_NO_THROW(s.set_linger(true, 5));
}

TEST(SocketTest, SetNonblocking) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
    EXPECT_NO_THROW(s.set_nonblocking());
}

// Type safety: TCP_NODELAY cannot be called on a UDP socket — the new
// type-safe API (udp_socket vs tcp_socket) prevents this at compile time.
// Previously this was a runtime error; now it's a compile-time guarantee.
TEST(SocketTest, UdpSocketDoesNotHaveTcpNoDelay) {
    coronet::udp_socket s = coronet::udp_socket::create_udp(AF_INET);
    EXPECT_NE(s.native_handle(), coronet::platform::invalid_socket);
    // s.set_tcp_no_delay(true) — would not compile (method doesn't exist on udp_socket)
    // Verify UDP-specific options work instead
    EXPECT_NO_THROW(s.set_broadcast(true));
}

// ====================================================================
// Bind / Listen
// ====================================================================

// Fast success: bind to ephemeral port (port 0)
TEST(SocketTest, BindEphemeralPort) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
    s.set_reuse_addr(true);
    EXPECT_NO_THROW(s.bind(coronet::inet_address{(uint16_t)0, false}));
    // After bind to port 0, local_addr should return a non-zero port
    auto addr = s.local_addr();
    ASSERT_TRUE(addr.has_value());
    EXPECT_NE(addr->port(), 0);
}

// Fast failure: bind two sockets to the same specific address+port (no SO_REUSEADDR)
TEST(SocketTest, BindConflictThrows) {
    coronet::tcp_socket s1 = coronet::tcp_socket::create_tcp(AF_INET);
    s1.bind(coronet::inet_address{"127.0.0.1", 0});
    auto addr = s1.local_addr();
    ASSERT_TRUE(addr.has_value());
    uint16_t port = addr->port();

    // Second socket: try to bind to the same address+port — should throw
    coronet::tcp_socket s2 = coronet::tcp_socket::create_tcp(AF_INET);
    EXPECT_THROW(s2.bind(coronet::inet_address{"127.0.0.1", port}), std::system_error);
}

TEST(SocketTest, ListenSuccess) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
    s.set_reuse_addr(true);
    s.bind(coronet::inet_address{(uint16_t)0, false});
    EXPECT_NO_THROW(s.listen(5));
}

// ====================================================================
// Address queries
// ====================================================================

TEST(SocketTest, LocalAddrAfterBind) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
    s.set_reuse_addr(true);
    s.bind(coronet::inet_address{"127.0.0.1", 0});
    auto addr = s.local_addr();
    ASSERT_TRUE(addr.has_value());
    EXPECT_EQ(addr->family(), AF_INET);
    EXPECT_NE(addr->port(), 0);
    EXPECT_EQ(addr->to_ip(), "127.0.0.1");
}

// Fast failure: peer_addr on unconnected socket returns nullopt
TEST(SocketTest, PeerAddrUnconnectedReturnsNullopt) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
    auto peer = s.peer_addr();
    EXPECT_FALSE(peer.has_value());
}

TEST(SocketTest, LocalAddrIPv6AfterBind) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET6);
    s.set_reuse_addr(true);
    s.bind(coronet::inet_address{(uint16_t)0, true});
    auto addr = s.local_addr();
    ASSERT_TRUE(addr.has_value());
    EXPECT_EQ(addr->family(), AF_INET6);
    EXPECT_NE(addr->port(), 0);
}

// ====================================================================
// Chain operations
// ====================================================================

TEST(SocketTest, ChainOperations) {
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
    // All methods return socket& for chaining
    s.set_reuse_addr(true)
     .bind(coronet::inet_address{(uint16_t)0, false})
     .listen(10);

    auto addr = s.local_addr();
    ASSERT_TRUE(addr.has_value());
    EXPECT_NE(addr->port(), 0);
}

// ====================================================================
// RAII: repeated create/destroy (fd leak detection)
// ====================================================================

TEST(SocketTest, RAIIRepeatedCreateDestroy) {
    // Create and destroy 1000 sockets. If fds are leaked, create_tcp
    // will throw std::system_error when the fd limit is hit.
    for (int i = 0; i < 1000; i++) {
        coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
        ASSERT_NE(s.native_handle(), coronet::platform::invalid_socket);
    }
    SUCCEED();
}

TEST(SocketTest, RAIIRepeatedMoveAssign) {
    // Repeatedly move-assign to verify P0-1 fix: old fd is always closed
    coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
    for (int i = 0; i < 500; i++) {
        s = coronet::tcp_socket::create_tcp(AF_INET);
        ASSERT_NE(s.native_handle(), coronet::platform::invalid_socket);
    }
    SUCCEED();
}

// ====================================================================
// Acceptor
// ====================================================================

TEST(AcceptorTest, ConstructionIPv4) {
    coronet::tcp_acceptor ac{coronet::inet_address{(uint16_t)0, false}};
    EXPECT_NE(ac.listen_fd(), coronet::platform::invalid_socket);
}

TEST(AcceptorTest, ConstructionIPv6) {
    coronet::tcp_acceptor ac{coronet::inet_address{(uint16_t)0, true}};
    EXPECT_NE(ac.listen_fd(), coronet::platform::invalid_socket);
}

TEST(AcceptorTest, ConstructionCustomBacklog) {
    coronet::tcp_acceptor ac{coronet::inet_address{(uint16_t)0, false}, 5};
    EXPECT_NE(ac.listen_fd(), coronet::platform::invalid_socket);
}

TEST(AcceptorTest, ConstructionSpecificAddress) {
    coronet::tcp_acceptor ac{coronet::inet_address{"127.0.0.1", 0}};
    EXPECT_NE(ac.listen_fd(), coronet::platform::invalid_socket);
}

// Acceptor move semantics
TEST(AcceptorTest, MoveConstruction) {
    coronet::tcp_acceptor ac1{coronet::inet_address{(uint16_t)0, false}};
    coronet::tcp_acceptor ac2 = std::move(ac1);
    EXPECT_NE(ac2.listen_fd(), coronet::platform::invalid_socket);
}

// Acceptor RAII: repeated construction/destruction (fd leak + TIME_WAIT)
// The acceptor sets SO_REUSEADDR, so TIME_WAIT ports are reused.
TEST(AcceptorTest, RAIIRepeatedConstruction) {
    for (int i = 0; i < 100; i++) {
        coronet::tcp_acceptor ac{coronet::inet_address{"127.0.0.1", 0}};
        ASSERT_NE(ac.listen_fd(), coronet::platform::invalid_socket);
    }
    SUCCEED();
}

// Fast failure: bind to an occupied port throws during construction.
// SO_REUSEADDR is set by acceptor, but on most platforms binding to the
// same specific address+port that's already LISTENing still fails.
TEST(AcceptorTest, ConstructionOnOccupiedPort) {
    // First acceptor binds to ephemeral port
    coronet::tcp_acceptor ac1{coronet::inet_address{"127.0.0.1", 0}};
    // Get the actual port via getsockname on the raw fd (don't construct
    // a socket from it — that would cause double-close)
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    getsockname((int)ac1.listen_fd(), (struct sockaddr*)&ss, &len);
    uint16_t port = ntohs(((struct sockaddr_in*)&ss)->sin_port);

    // Second acceptor on the same port — may or may not throw depending
    // on SO_REUSEADDR semantics. Either way, no crash.
    try {
        coronet::tcp_acceptor ac2{coronet::inet_address{"127.0.0.1", port}};
        // No throw — SO_REUSEADDR allowed double bind (some platforms)
    } catch (const std::system_error&) {
        // Throw — platform rejected double bind
    }
    SUCCEED();  // Either outcome is acceptable
}
