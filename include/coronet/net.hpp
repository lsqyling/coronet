#pragma once

// Network abstractions: socket, acceptor, inet_address
//
// 网络抽象头文件 —— 聚合 includes，用户只需包含此文件即可使用所有网络功能。
//
// ## 包含关系
//
//   net.hpp
//     ├── inet_address.hpp   — IPv4/IPv6 套接字地址 + DNS 解析
//     ├── socket.hpp         — TCP/UDP 套接字 RAII 封装 + 异步 I/O
//     └── acceptor.hpp       — TCP 连接接收器（监听 + accept）
//
// ## 使用方式
//
//   #include <coronet/net.hpp>
//
//   即可使用 socket、acceptor、inet_address 三个核心类型。
//   异步 I/O 操作（recv/send/accept/connect 等）通过 socket 的方法透明调用 async:: 工厂。
#include "coronet/net/inet_address.hpp"
#include "coronet/net/socket.hpp"
#include "coronet/net/acceptor.hpp"
