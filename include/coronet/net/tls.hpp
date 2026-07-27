#pragma once

// ============================================================
// tls.hpp — TLS 模块聚合头文件
// ============================================================
//
// 用户只需包含此文件即可使用所有 TLS 功能。
// 前置条件：CORONET_HAS_TLS 定义（通过 cmake -DCORONET_WITH_TLS=ON 启用）
//
// ## 包含关系
//
//   tls.hpp
//     ├── tls_context.hpp    — SSL_CTX 上下文（证书、密钥、ALPN）
//     ├── tls_socket.hpp     — TLS 套接字（包装 tcp_socket + SSL*）
//     └── tls_acceptor.hpp   — TLS 监听器（包装 tcp_acceptor + 握手）
//
// ## 核心类型
//
//   - tls_context   — TLS 配置上下文（可被多个连接共享）
//   - tls_socket    — TLS 加密套接字（满足 transport concept）
//   - tls_acceptor  — TLS 监听器（满足 listener concept）
//
// ## 使用方式
//
//   #include <coronet/net/tls.hpp>
//
//   // 服务端
//   tls_context ctx{tls_context::mode::server};
//   ctx.load_cert_file("server.crt", "server.key");
//   ctx.set_alpn({"http/1.1"});
//
//   tls_acceptor ac{inet_address{443}, ctx};
//   auto conn = co_await ac.accept_socket();
//
//   // 客户端
//   tls_context ctx{tls_context::mode::client};
//   ctx.set_verify_peer(true);
//   ctx.set_default_verify_paths();
//
//   auto sock = co_await tls_socket::connect(addr, ctx);
//
// ## 传输层抽象
//
//   tls_socket 满足 transport concept，可与 tcp_socket 互换：
//
//   template<coronet::transport T>
//   task<void> echo_session(T conn) {
//       char buf[4096];
//       while (int n = co_await conn.recv(buf)) {
//           co_await conn.send({buf, (size_t)n});
//       }
//   }

#ifdef CORONET_HAS_TLS

#include "coronet/net/tls/tls_context.hpp"
#include "coronet/net/tls/tls_socket.hpp"
#include "coronet/net/tls/tls_acceptor.hpp"

#else
#error "coronet/net/tls.hpp requires CORONET_HAS_TLS. \
Build with cmake -DCORONET_WITH_TLS=ON and ensure OpenSSL is installed."
#endif
