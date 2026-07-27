#pragma once

// ============================================================
// net.hpp — 网络模块聚合头文件
// ============================================================
//
// 用户只需包含此文件即可使用所有网络功能。
//
// ## 包含关系
//
//   net.hpp
//     ├── inet_address.hpp    — IPv4/IPv6 套接字地址 + DNS 解析
//     ├── transport.hpp       — C++20 concepts (transport / datagram / listener)
//     ├── socket_base.hpp     — 套接字基类（RAII + 公共选项）
//     ├── tcp_socket.hpp      — TCP 套接字（面向连接的可靠数据流）
//     ├── tcp_acceptor.hpp    — TCP 连接接收器（监听 + accept）
//     ├── udp_socket.hpp      — UDP 套接字（无连接数据报传输）
//     └── tls.hpp (可选)      — TLS 加密传输（需 CORONET_WITH_TLS=ON）
//
// ## 核心类型
//
//   - inet_address   — IP 地址封装
//   - tcp_socket     — TCP 套接字（满足 transport concept）
//   - tcp_acceptor   — TCP 监听器（满足 listener concept）
//   - udp_socket     — UDP 套接字（满足 datagram concept）
//   - socket_base    — 套接字基类（不直接使用）
//   - tls_context    — TLS 上下文（可选，需 OpenSSL）
//   - tls_socket     — TLS 套接字（可选，满足 transport concept）
//   - tls_acceptor   — TLS 监听器（可选，满足 listener concept）
//
// ## 向后兼容
//
//   using socket   = tcp_socket;     // 旧代码中的 socket 仍可用
//   using acceptor = tcp_acceptor;   // 旧代码中的 acceptor 仍可用
//
// ## 使用方式
//
//   #include <coronet/net.hpp>
//
//   TCP 服务端:
//     tcp_acceptor ac{inet_address{8080}};
//     auto conn = co_await ac.accept_socket();
//
//   TCP 客户端:
//     tcp_socket sock = tcp_socket::create(AF_INET);
//     co_await sock.connect(addr);
//
//   UDP 收发:
//     udp_socket sock = udp_socket::create(AF_INET);
//     sock.bind(inet_address{9000});
//     auto [n, peer] = co_await sock.recvfrom(buf);

#include "coronet/net/inet_address.hpp"
#include "coronet/net/transport.hpp"
#include "coronet/net/socket_base.hpp"
#include "coronet/net/tcp_socket.hpp"
#include "coronet/net/tcp_acceptor.hpp"
#include "coronet/net/udp_socket.hpp"

// TLS 模块（可选：需 CORONET_WITH_TLS=ON + OpenSSL）
#ifdef CORONET_HAS_TLS
#include "coronet/net/tls.hpp"
#endif
