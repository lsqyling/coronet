#pragma once

// ============================================================
// acceptor.hpp — 向后兼容 shim（已迁移到 tcp_acceptor.hpp）
// ============================================================
//
// 此文件仅用于向后兼容。新的代码应直接包含：
//   #include <coronet/net/tcp_acceptor.hpp>
//
// 旧代码中的 `coronet::acceptor` 仍可用 — 它是 `tcp_acceptor` 的类型别名。

#include "coronet/net/tcp_acceptor.hpp"
