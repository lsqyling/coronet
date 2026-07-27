#pragma once
// ============================================================
// proactor_selector.hpp — 平台 Proactor 选择的唯一汇聚点
// ============================================================
// 全库唯一的编译期平台 Proactor 选择处。所有需要 proactor 具体类型的
// 上层（io_context / worker_meta）都应 include 本文件，而非直接 include
// 各平台的具体 proactor 实现头。
//
// 这样做的好处：
//   - 新增/切换平台只需修改本文件一处（单一真相源）
//   - 上层不再与具体平台实现头直接耦合，依赖边界清晰
//   - platform::proactor_type 是唯一的 proactor 类型别名
//
// 注意：静态多态（编译期选择、proactor 为值成员、零虚表）保持不变，
// 本文件只是把原本散布在 io_context.hpp / worker_meta.hpp 中的三份
// 重复 #ifdef 选择逻辑集中到一处。
//
// Single source of truth for compile-time platform proactor selection.
// Upper layers include this bridge instead of concrete proactor headers.

#include "coronet/platform/platform.hpp"

#if defined(CORONET_PLATFORM_WINDOWS)
  #include "coronet/platform/iocp/iocp_proactor.hpp"
  namespace coronet::platform {
      /// 编译期选定的 Proactor 具体类型（Windows = IOCP）
      using proactor_type = iocp::iocp_proactor;
  }
#elif defined(CORONET_USE_IOURING)
  #include "coronet/platform/io_uring/io_uring_proactor.hpp"
  namespace coronet::platform {
      /// 编译期选定的 Proactor 具体类型（Linux + io_uring）
      using proactor_type = io_uring::io_uring_proactor;
  }
#else
  #include "coronet/platform/epoll/epoll_reactor.hpp"
  namespace coronet::platform {
      /// 编译期选定的 Proactor 具体类型（Linux 默认 = epoll）
      using proactor_type = epoll::epoll_proactor;
  }
#endif
