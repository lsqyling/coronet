#include "coronet/net/socket.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(CORONET_PLATFORM_LINUX)
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#elif defined(CORONET_PLATFORM_WINDOWS)
#include <winsock2.h>
#endif

namespace coronet {

socket& socket::bind(const inet_address& addr) {
    int ret = ::bind((int)sockfd_, addr.get_sockaddr(), addr.length());
    if (ret < 0) {
        std::perror("socket::bind");
        std::abort();
    }
    return *this;
}

socket& socket::listen(int backlog) {
    int ret = ::listen((int)sockfd_, backlog);
    if (ret < 0) {
        std::perror("socket::listen");
        std::abort();
    }
    return *this;
}

socket& socket::set_reuse_addr(bool on) {
    int optval = on ? 1 : 0;
    int ret = ::setsockopt((int)sockfd_, SOL_SOCKET, SO_REUSEADDR,
                           (const char*)&optval, sizeof(optval));
    if (ret < 0) {
        std::perror("socket::set_reuse_addr");
    }
    return *this;
}

socket& socket::set_tcp_no_delay(bool on) {
    int optval = on ? 1 : 0;
    int ret = ::setsockopt((int)sockfd_, IPPROTO_TCP, TCP_NODELAY,
                           (const char*)&optval, sizeof(optval));
    if (ret < 0) {
        std::perror("socket::set_tcp_no_delay");
    }
    return *this;
}

void socket::set_nonblocking() {
#if defined(CORONET_PLATFORM_LINUX)
    int flags = ::fcntl((int)sockfd_, F_GETFL, 0);
    ::fcntl((int)sockfd_, F_SETFL, flags | O_NONBLOCK);
#elif defined(CORONET_PLATFORM_WINDOWS)
    u_long mode = 1;
    ::ioctlsocket((SOCKET)sockfd_, FIONBIO, &mode);
#endif
}

inet_address socket::local_addr() const {
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (::getsockname((int)sockfd_, (struct sockaddr*)&ss, &len) < 0) {
        std::perror("socket::local_addr");
        return inet_address{};
    }
    return inet_address{*reinterpret_cast<const struct sockaddr*>(&ss)};
}

inet_address socket::peer_addr() const {
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (::getpeername((int)sockfd_, (struct sockaddr*)&ss, &len) < 0) {
        std::perror("socket::peer_addr");
        return inet_address{};
    }
    return inet_address{*reinterpret_cast<const struct sockaddr*>(&ss)};
}

} // namespace coronet
