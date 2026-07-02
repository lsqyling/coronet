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

/// 绑定套接字到指定地址。
///
/// 同步绑定操作。失败时输出错误并终止（简化错误处理，示例代码风格）。
/// 生产环境应改用错误码返回。
socket& socket::bind(const inet_address& addr) {
    int ret = ::bind((int)sockfd_, addr.get_sockaddr(), addr.length());
    if (ret < 0) {
        std::perror("socket::bind");
        std::abort();
    }
    return *this;
}

/// 开始监听传入连接。
///
/// backlog 参数控制连接等待队列的最大长度。
/// 超出此长度的连接请求将被内核拒绝（TCP 层面）。
socket& socket::listen(int backlog) {
    int ret = ::listen((int)sockfd_, backlog);
    if (ret < 0) {
        std::perror("socket::listen");
        std::abort();
    }
    return *this;
}

/// 设置 SO_REUSEADDR 选项 —— 允许重用 TIME_WAIT 状态的地址。
///
/// 服务器重启时，如果之前的连接仍处于 TIME_WAIT 状态，
/// 设置此选项可以避免 "Address already in use" 错误。
socket& socket::set_reuse_addr(bool on) {
    int optval = on ? 1 : 0;
    int ret = ::setsockopt((int)sockfd_, SOL_SOCKET, SO_REUSEADDR,
                           (const char*)&optval, sizeof(optval));
    if (ret < 0) {
        std::perror("socket::set_reuse_addr");
    }
    return *this;
}

/// 设置 TCP_NODELAY —— 禁用 Nagle 算法。
///
/// Nagle 算法会合并小数据包，在交互式场景（如 Telnet）中减少网络包数量。
/// 但对于需要低延迟的网络服务（如 HTTP API 响应），禁用 Nagle 算法
/// 可以避免发送的延迟。尤其适合请求-响应模式。
socket& socket::set_tcp_no_delay(bool on) {
    int optval = on ? 1 : 0;
    int ret = ::setsockopt((int)sockfd_, IPPROTO_TCP, TCP_NODELAY,
                           (const char*)&optval, sizeof(optval));
    if (ret < 0) {
        std::perror("socket::set_tcp_no_delay");
    }
    return *this;
}

/// 设置非阻塞模式。
///
/// 不同平台的实现方式：
///   Linux:   fcntl F_SETFL + O_NONBLOCK
///   Windows: ioctlsocket + FIONBIO
///
/// 注意：对于使用 create_tcp/create_udp 创建的套接字，
/// Linux 上已经设置了 SOCK_NONBLOCK，无需额外调用。
/// 此方法主要用于从 accept 获取的套接字。
void socket::set_nonblocking() {
#if defined(CORONET_PLATFORM_LINUX)
    int flags = ::fcntl((int)sockfd_, F_GETFL, 0);
    ::fcntl((int)sockfd_, F_SETFL, flags | O_NONBLOCK);
#elif defined(CORONET_PLATFORM_WINDOWS)
    u_long mode = 1;
    ::ioctlsocket((SOCKET)sockfd_, FIONBIO, &mode);
#endif
}

/// 获取本地地址（getsockname）。
inet_address socket::local_addr() const {
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (::getsockname((int)sockfd_, (struct sockaddr*)&ss, &len) < 0) {
        std::perror("socket::local_addr");
        return inet_address{};
    }
    return inet_address{*reinterpret_cast<const struct sockaddr*>(&ss)};
}

/// 获取对端地址（getpeername）。
///
/// 对于 TCP 连接，返回已连接的对端地址。
/// 对于未连接的套接字，行为未定义。
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
