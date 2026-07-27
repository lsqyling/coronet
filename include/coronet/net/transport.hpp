#pragma once

// ============================================================
// transport.hpp — 传输层抽象概念 (C++20 concepts)
// ============================================================
// 定义 transport 和 datagram 概念，为 TLS / QUIC 提供扩展点。
//
// 设计哲学：静态多态、零虚表开销
//   - 用 concept 约束模板参数，编译期保证接口完整
//   - 不同传输类型（tcp_socket / tls_socket / quic_stream）通过
//     模板参数传递，编译器内联所有调用
//   - 用户可以编写 transport<T> 泛型代码，T 可以是任意满足概念的类型
//
// 概念层次：
//   transport  — 面向连接的数据流传输（TCP / TLS / QUIC stream）
//   datagram   — 无连接数据报传输（UDP）
//   listener   — 连接接受器（TCP acceptor / TLS acceptor / QUIC listener）
//
// 使用示例：
//   template<coronet::transport T>
//   task<void> echo_session(T conn) {
//       char buf[4096];
//       while (int n = co_await conn.recv(buf)) {
//           co_await conn.send({buf, (size_t)n});
//       }
//   }
//
// 未来扩展：
//   Phase 3: tls_socket 满足 transport concept（包装 tcp_socket + BoringSSL）
//   Phase 4: quic_stream 满足 transport concept（quic_engine UDP 多路复用）

#include "coronet/net/inet_address.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

namespace coronet {

/// 面向连接的传输概念 — 适用于 TCP / TLS / QUIC stream。
///
/// 所有面向连接的传输类型必须满足此概念。
/// 通过 recv/send 进行双向数据流通信，支持半关闭和异步关闭。
///
/// Requirements:
///   - recv(span<char>) → awaitable (co_await 得到 int，>0 字节数，0 EOF，<0 错误)
///   - send(span<const char>) → awaitable (co_await 得到 int)
///   - close() → awaitable
///   - shutdown_write() → awaitable
///   - native_handle() → 平台原生句柄
template<typename T>
concept transport = requires(T t, std::span<char> rbuf, std::span<const char> wbuf) {
    { t.recv(rbuf) };
    { t.send(wbuf) };
    { t.close() };
    { t.shutdown_write() };
    { t.native_handle() } -> std::convertible_to<std::uintptr_t>;
};

/// 无连接数据报传输概念 — 适用于 UDP。
///
/// UDP 是无连接的，每个数据报可以来自/发往不同地址。
/// recvfrom 返回数据报内容和源地址，sendto 指定目标地址。
///
/// Requirements:
///   - recvfrom(span<char>) → task<udp_packet> (co_await 得到 {bytes, peer})
///   - sendto(span<const char>, const inet_address&) → awaitable
template<typename T>
concept datagram = requires(T t, std::span<char> rbuf, std::span<const char> wbuf,
                            const inet_address& addr) {
    { t.recvfrom(rbuf) };
    { t.sendto(wbuf, addr) };
};

/// 监听器概念 — 适用于 TCP acceptor / TLS acceptor / QUIC listener。
///
/// 监听器通过 accept 生成已连接的传输对象。
///
/// Requirements:
///   - accept_socket() → task<满足 transport 概念的对象>
template<typename T>
concept listener = requires(T l) {
    { l.accept_socket() };
};

} // namespace coronet
