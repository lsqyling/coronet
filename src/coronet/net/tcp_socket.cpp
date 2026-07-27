#include "coronet/net/tcp_socket.hpp"

#include <system_error>

#if defined(CORONET_PLATFORM_LINUX)
#include <netinet/tcp.h>
#include <sys/socket.h>
#elif defined(CORONET_PLATFORM_WINDOWS)
#include <winsock2.h>
#endif

namespace coronet {

/// 开始监听传入连接。
///
/// backlog 参数控制连接等待队列的最大长度。
/// 超出此长度的连接请求将被内核拒绝（TCP 层面）。
///
/// 失败时抛出 std::system_error 而非 std::abort()。
tcp_socket& tcp_socket::listen(int backlog) {
    int ret = ::listen((int)sockfd_, backlog);
    if (ret < 0) {
        throw std::system_error(
#if defined(CORONET_PLATFORM_WINDOWS)
            WSAGetLastError(), std::system_category(), "tcp_socket::listen"
#else
            errno, std::generic_category(), "tcp_socket::listen"
#endif
        );
    }
    return *this;
}

/// 设置 TCP_NODELAY — 禁用 Nagle 算法。
///
/// Nagle 算法会合并小数据包，在交互式场景中减少网络包数量。
/// 但对于需要低延迟的网络服务（如 HTTP API 响应），
/// 禁用 Nagle 算法可以避免发送延迟。尤其适合请求-响应模式。
tcp_socket& tcp_socket::set_tcp_no_delay(bool on) {
    int optval = on ? 1 : 0;
    int ret = ::setsockopt((int)sockfd_, IPPROTO_TCP, TCP_NODELAY,
                           (const char*)&optval, sizeof(optval));
    if (ret < 0) {
        throw std::system_error(
#if defined(CORONET_PLATFORM_WINDOWS)
            WSAGetLastError(), std::system_category(), "tcp_socket::set_tcp_no_delay"
#else
            errno, std::generic_category(), "tcp_socket::set_tcp_no_delay"
#endif
        );
    }
    return *this;
}

/// 设置 SO_KEEPALIVE — 启用 TCP 保活机制。
///
/// 当连接空闲时定期发送保活探测包，检测对端是否存活。
/// 适用于长连接场景，避免半开连接永久占用资源。
tcp_socket& tcp_socket::set_keepalive(bool on) {
    int optval = on ? 1 : 0;
    int ret = ::setsockopt((int)sockfd_, SOL_SOCKET, SO_KEEPALIVE,
                           (const char*)&optval, sizeof(optval));
    if (ret < 0) {
        throw std::system_error(
#if defined(CORONET_PLATFORM_WINDOWS)
            WSAGetLastError(), std::system_category(), "tcp_socket::set_keepalive"
#else
            errno, std::generic_category(), "tcp_socket::set_keepalive"
#endif
        );
    }
    return *this;
}

} // namespace coronet
