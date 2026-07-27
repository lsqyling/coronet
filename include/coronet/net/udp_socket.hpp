#pragma once

// ============================================================
// udp_socket.hpp — UDP 套接字（无连接数据报传输）
// ============================================================
// 继承 socket_base，添加 UDP 特有操作：
//   - recvfrom / sendto — 无连接数据报收发（核心操作）
//   - recv / send — 已连接 UDP 的收发（connect 后限定对端）
//   - connect (sync) — 设置默认对端（非 TCP 的 3 次握手，仅内核设置）
//   - set_broadcast — 广播支持
//   - multicast — 组播支持（join_group / leave_group / set_multicast_ttl / set_multicast_loop）
//
// 音视频应用场景：
//   - VoIP / 视频会议：低延迟 RTP/RTCP over UDP
//   - 直播：组播分发
//   - DNS 查询：小型数据报
//   - IoT：轻量通信
//
// 满足 datagram concept。
//
// 使用示例：
//   // 服务端（接收端）
//   udp_socket sock = udp_socket::create_udp(AF_INET);
//   sock.bind(inet_address{9000});
//   char buf[1500];
//   auto [n, peer] = co_await sock.recvfrom(buf);
//   // 处理数据...
//   co_await sock.sendto(reply, peer);
//
//   // 客户端（发送端）
//   udp_socket sock = udp_socket::create_udp(AF_INET);
//   co_await sock.sendto(data, inet_address{"224.0.0.1", 9000});
//
//   // 已连接 UDP（限定单一对端）
//   udp_socket sock = udp_socket::create_udp(AF_INET);
//   sock.connect(peer_addr);  // 同步，仅设置默认对端
//   co_await sock.send(data); // 发送到默认对端
//   int n = co_await sock.recv(buf); // 只接收来自默认对端的数据报

#include "coronet/net/socket_base.hpp"
#include "coronet/task.hpp"

#include <system_error>

#if defined(CORONET_PLATFORM_WINDOWS)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace coronet {

/// UDP recvfrom 便捷返回类型 — 字节数 + 对端地址。
///
/// recvfrom_raw() 返回平台层的 recvfrom_result（含 raw sockaddr_storage）。
/// recvfrom() 返回此结构（含 inet_address），通过协程包装，开销可忽略。
struct udp_packet {
    int bytes;           // >0: 接收到的字节数; 0: 空数据报; <0: 错误
    inet_address peer;   // 数据报源地址
};

/// UDP 套接字 — 无连接数据报传输。
///
/// 满足 datagram concept。
///
/// 与 tcp_socket 的关键区别：
///   1. 无连接 — 每个数据报独立，可来自/发往不同地址
///   2. 不可靠 — 不保证送达、顺序、无重复
///   3. 低延迟 — 无握手、无重传、无拥塞控制
///   4. 支持广播/组播 — 一对多分发
class udp_socket : public socket_base<udp_socket> {
public:
    /// 从 fd 构造（public — 供工厂方法、外部代码使用）
    explicit udp_socket(int fd) noexcept : socket_base<udp_socket>(fd) {}

    // 移动语义（默认 — 调用基类的移动操作）
    udp_socket(udp_socket&&) noexcept = default;
    udp_socket& operator=(udp_socket&&) noexcept = default;

    // ---- UDP 特有同步操作 ----

    /// 连接到指定对端（UDP connect — 仅设置默认对端，不发起网络通信）。
    ///
    /// UDP 的 connect 是同步操作：内核仅记录默认对端地址，
    /// 后续 send() 发送到此地址，recv() 只接收来自此地址的数据报。
    /// 不发起 3 次握手（与 TCP connect 不同）。
    ///
    /// @throws std::system_error 如果 ::connect 失败
    void connect(const inet_address& addr);

    /// 设置 SO_BROADCAST — 允许发送广播
    udp_socket& set_broadcast(bool on);

    // ---- 组播（Multicast）----

    /// 加入组播组
    /// @param group 组播地址（如 "239.0.0.1"）
    /// @param iface 接口地址（默认 INADDR_ANY，由系统选择接口）
    udp_socket& join_group(const inet_address& group,
                           const inet_address& iface = inet_address{(uint16_t)0, false});

    /// 离开组播组
    udp_socket& leave_group(const inet_address& group,
                            const inet_address& iface = inet_address{(uint16_t)0, false});

    /// 设置组播 TTL（1=本地网络，<=32=同一站点，>32=更广范围）
    udp_socket& set_multicast_ttl(int ttl);

    /// 设置组播回环（是否接收自己发送的组播包）
    udp_socket& set_multicast_loop(bool on);

    // ---- UDP 异步操作 ----

    /// 异步接收数据报（零开销，返回平台 awaiter）。
    ///
    /// 返回平台层 awaiter，co_await 得到 recvfrom_result{bytes, sockaddr_storage}。
    /// 用户自行从 sockaddr_storage 构造 inet_address。
    /// 适用于高频音视频场景，避免协程帧分配。
    ///
    /// @code
    /// auto raw = co_await sock.recvfrom_raw(buf);
    /// inet_address peer{reinterpret_cast<const sockaddr&>(raw.addr)};
    /// @endcode
    [[nodiscard("Did you forget to co_await?")]]
    auto recvfrom_raw(std::span<char> buf, int flags = 0) noexcept {
        return async::recvfrom((int)sockfd_, buf, flags);
    }

    /// 异步接收数据报（便捷版，返回 udp_packet 含 inet_address）。
    ///
    /// 通过协程包装 recvfrom_raw，将 sockaddr_storage 转为 inet_address。
    /// 开销：一个协程帧分配（~64 bytes），对 UDP 音视频可忽略。
    ///
    /// @code
    /// auto [n, peer] = co_await sock.recvfrom(buf);
    /// @endcode
    [[nodiscard("Did you forget to co_await?")]]
    task<udp_packet> recvfrom(std::span<char> buf, int flags = 0) {
        auto raw = co_await async::recvfrom((int)sockfd_, buf, flags);
        co_return udp_packet{
            static_cast<int>(raw.bytes),
            inet_address{reinterpret_cast<const struct sockaddr&>(raw.addr)}
        };
    }

    /// 异步发送数据报到指定地址
    [[nodiscard("Did you forget to co_await?")]]
    auto sendto(std::span<const char> buf, const inet_address& dest, int flags = 0) noexcept {
        return async::sendto((int)sockfd_, buf,
                             dest.get_sockaddr(), dest.length(), flags);
    }

    /// 异步接收（已连接 UDP — connect 后使用）
    [[nodiscard("Did you forget to co_await?")]]
    auto recv(std::span<char> buf, int flags = 0) noexcept {
        return async::recv((int)sockfd_, buf, flags);
    }

    /// 异步发送（已连接 UDP — connect 后使用）
    [[nodiscard("Did you forget to co_await?")]]
    auto send(std::span<const char> buf, int flags = 0) noexcept {
        return async::send((int)sockfd_, buf, flags);
    }

    // ---- 工厂方法 ----

    /// 创建非阻塞 UDP 套接字
    static udp_socket create_udp(sa_family_t family);
    /// create 的别名
    static udp_socket create(sa_family_t family) { return create_udp(family); }
};

// ---- 内联实现 ----

inline udp_socket udp_socket::create_udp(sa_family_t family) {
#if defined(CORONET_PLATFORM_LINUX)
    int fd = ::socket(family, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_UDP);
#else
    int fd = (int)::WSASocketW(family, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0,
                               WSA_FLAG_OVERLAPPED);
#endif
    if (fd < 0)
        throw std::system_error(
#if defined(CORONET_PLATFORM_WINDOWS)
            WSAGetLastError(), std::system_category(), "create_udp"
#else
            errno, std::generic_category(), "create_udp"
#endif
        );
    return udp_socket{fd};
}

inline void udp_socket::connect(const inet_address& addr) {
    int ret = ::connect((int)sockfd_, addr.get_sockaddr(), addr.length());
    if (ret < 0) {
        throw std::system_error(
#if defined(CORONET_PLATFORM_WINDOWS)
            WSAGetLastError(), std::system_category(), "udp_socket::connect"
#else
            errno, std::generic_category(), "udp_socket::connect"
#endif
        );
    }
}

inline udp_socket& udp_socket::set_broadcast(bool on) {
    int optval = on ? 1 : 0;
    int ret = ::setsockopt((int)sockfd_, SOL_SOCKET, SO_BROADCAST,
                           reinterpret_cast<const char*>(&optval), sizeof(optval));
    if (ret < 0) {
        throw std::system_error(
#if defined(CORONET_PLATFORM_WINDOWS)
            WSAGetLastError(), std::system_category(), "udp_socket::set_broadcast"
#else
            errno, std::generic_category(), "udp_socket::set_broadcast"
#endif
        );
    }
    return *this;
}

inline udp_socket& udp_socket::join_group(const inet_address& group,
                                           const inet_address& iface) {
    if (group.family() == AF_INET) {
        struct ip_mreq mreq;
        mreq.imr_multiaddr = reinterpret_cast<const struct sockaddr_in&>(group).sin_addr;
        mreq.imr_interface = reinterpret_cast<const struct sockaddr_in&>(iface).sin_addr;
        int ret = ::setsockopt((int)sockfd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                               reinterpret_cast<const char*>(&mreq), sizeof(mreq));
        if (ret < 0) {
            throw std::system_error(
#if defined(CORONET_PLATFORM_WINDOWS)
                WSAGetLastError(), std::system_category(), "udp_socket::join_group"
#else
                errno, std::generic_category(), "udp_socket::join_group"
#endif
            );
        }
    } else if (group.family() == AF_INET6) {
        struct ipv6_mreq mreq6;
        mreq6.ipv6mr_multiaddr = reinterpret_cast<const struct sockaddr_in6&>(group).sin6_addr;
        mreq6.ipv6mr_interface = 0;
        int ret = ::setsockopt((int)sockfd_, IPPROTO_IPV6, IPV6_JOIN_GROUP,
                               reinterpret_cast<const char*>(&mreq6), sizeof(mreq6));
        if (ret < 0) {
            throw std::system_error(
#if defined(CORONET_PLATFORM_WINDOWS)
                WSAGetLastError(), std::system_category(), "udp_socket::join_group6"
#else
                errno, std::generic_category(), "udp_socket::join_group6"
#endif
            );
        }
    }
    return *this;
}

inline udp_socket& udp_socket::leave_group(const inet_address& group,
                                            const inet_address& iface) {
    if (group.family() == AF_INET) {
        struct ip_mreq mreq;
        mreq.imr_multiaddr = reinterpret_cast<const struct sockaddr_in&>(group).sin_addr;
        mreq.imr_interface = reinterpret_cast<const struct sockaddr_in&>(iface).sin_addr;
        int ret = ::setsockopt((int)sockfd_, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                               reinterpret_cast<const char*>(&mreq), sizeof(mreq));
        if (ret < 0) {
            throw std::system_error(
#if defined(CORONET_PLATFORM_WINDOWS)
                WSAGetLastError(), std::system_category(), "udp_socket::leave_group"
#else
                errno, std::generic_category(), "udp_socket::leave_group"
#endif
            );
        }
    } else if (group.family() == AF_INET6) {
        struct ipv6_mreq mreq6;
        mreq6.ipv6mr_multiaddr = reinterpret_cast<const struct sockaddr_in6&>(group).sin6_addr;
        mreq6.ipv6mr_interface = 0;
        int ret = ::setsockopt((int)sockfd_, IPPROTO_IPV6, IPV6_LEAVE_GROUP,
                               reinterpret_cast<const char*>(&mreq6), sizeof(mreq6));
        if (ret < 0) {
            throw std::system_error(
#if defined(CORONET_PLATFORM_WINDOWS)
                WSAGetLastError(), std::system_category(), "udp_socket::leave_group6"
#else
                errno, std::generic_category(), "udp_socket::leave_group6"
#endif
            );
        }
    }
    return *this;
}

inline udp_socket& udp_socket::set_multicast_ttl(int ttl) {
    int ret = ::setsockopt((int)sockfd_, IPPROTO_IP, IP_MULTICAST_TTL,
                           reinterpret_cast<const char*>(&ttl), sizeof(ttl));
    if (ret < 0) {
        throw std::system_error(
#if defined(CORONET_PLATFORM_WINDOWS)
            WSAGetLastError(), std::system_category(), "udp_socket::set_multicast_ttl"
#else
            errno, std::generic_category(), "udp_socket::set_multicast_ttl"
#endif
        );
    }
    return *this;
}

inline udp_socket& udp_socket::set_multicast_loop(bool on) {
    int optval = on ? 1 : 0;
    int ret = ::setsockopt((int)sockfd_, IPPROTO_IP, IP_MULTICAST_LOOP,
                           reinterpret_cast<const char*>(&optval), sizeof(optval));
    if (ret < 0) {
        throw std::system_error(
#if defined(CORONET_PLATFORM_WINDOWS)
            WSAGetLastError(), std::system_category(), "udp_socket::set_multicast_loop"
#else
            errno, std::generic_category(), "udp_socket::set_multicast_loop"
#endif
        );
    }
    return *this;
}

} // namespace coronet
