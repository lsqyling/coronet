#pragma once

// ============================================================
// socket.hpp — 向后兼容 shim（已迁移到 tcp_socket.hpp）
// ============================================================
//
// 此文件仅用于向后兼容。新的代码应直接包含：
//   #include <coronet/net/tcp_socket.hpp>
//
// 旧代码中的 `coronet::socket` 仍可用 — 它是 `tcp_socket` 的类型别名。
// 注意：socket::create_udp() 已移至 udp_socket::create_udp()。
//   旧: socket::create_udp(AF_INET)
//   新: udp_socket::create_udp(AF_INET)

#include "coronet/net/tcp_socket.hpp"
