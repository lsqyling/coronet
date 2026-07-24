============================================================
  Coronet vs ASIO — 最终测试报告
============================================================
  Date:     2026-07-24
  Platform: Windows 11 + MSVC 2022 Release build (native)
  Tool:     redis-benchmark (Windows x64 3.0.504)
  Command:  PING_INLINE (no pipelining)
============================================================

                      C1000K 压测结果
                    1,000,000 req × 1,000 concurrent
-----------------------------------------------------------------

  SINGLE-THREADED
  ┌────────────────────┬──────────┬────────┬──────┬────────┐
  │ Server             │ RPS      │ CPU%   │ Mem  │ Status │
  ├────────────────────┼──────────┼────────┼──────┼────────┤
  │ coronet_ST         │  50,955  │  28.4  │ 5MB  │  PASS  │
  │ coronet_chain      │  50,375  │  30.7  │ 5MB  │  PASS  │
  │ ASIO_ST            │  46,705  │  40.8  │ 5MB  │  PASS  │
  ├────────────────────┼──────────┼────────┼──────┼────────┤
  │ coronet vs ASIO    │ RPS +9%  │CPU 低30%│Mem持平│       │
  └────────────────────┴──────────┴────────┴──────┴────────┘

  MULTI-THREADED (6 threads)
  ┌────────────────────┬──────────┬────────┬──────┬────────┐
  │ Server             │ RPS      │ CPU%   │ Mem  │ Status │
  ├────────────────────┼──────────┼────────┼──────┼────────┤
  │ coronet_MT(6)      │  50,140  │  31.0  │ 6MB  │  PASS  │
  │ ASIO_MT(6)         │  30,071  │  80.4  │ 6MB  │  PASS  │
  ├────────────────────┼──────────┼────────┼──────┼────────┤
  │ coronet vs ASIO    │RPS +67%  │CPU 低61%│Mem持平│       │
  └────────────────────┴──────────┴────────┴──────┴────────┘

============================================================
  KEY FINDINGS
============================================================

1. 单线程: coronet_ST 领先 ASIO_ST
   - RPS 高 9.1% (50,955 vs 46,705)
   - CPU 低 12.4 个百分点 (28.4% vs 40.8%)
   - C++20 对称传输 + SPSC 无锁调度环的优势

2. 多线程: coronet_MT(6) 碾压 ASIO_MT(6)
   - RPS 高 66.7% (50,140 vs 30,071)
   - CPU 低 49.4 个百分点 (31.0% vs 80.4%)
   - ASIO 多线程存在严重的锁竞争和线程切换开销
   - coronet 的 co_spawn 跨线程 + SPSC ring 几乎无竞争

3. 链式调用: coronet_chain ≈ coronet_ST
   - RPS: 50,375 vs 50,955（差距 <1.2%）
   - 优化前链式比 ST 慢 2%，优化后基本持平
   - io_uring 后端采用 co_context 的 lazy_link_io 零开销模式
   - IOCP/epoll 后端采用 per-type CRTP 类型化分发

4. 稳定性: 零崩溃、零超时、零内存泄漏
   - 7 个 bug 全部修复
   - Windows CTest 22/22 + Linux 19/19 全部通过
   - C1000K 持续压测无内存增长

5. 内存: 所有 server 5-6MB，高效且一致
   - coronet_chain 之前的内存泄漏（256MB）已修复
   - iocp_operation 对象池回收机制保证稳定

============================================================
  BUGS FIXED (7 total)
============================================================

  #1 [致命] MSVC 协程运行时 use-after-free (task.hpp)
  #2 [致命] channel::buffer_end() past-the-end 解引用 UB (channel.hpp)
  #3 [中]   IOCP 缺少 ++requests_to_reap (iocp_win_io.hpp)
  #4 [低]   mutex.cpp std::assume_aligned MSVC LTO 风险 (mutex.cpp)
  #5 [中]   iocp_proactor::deinit() 后台线程竞态 (iocp_proactor.cpp)
  #6 [中]   win_accept 缺少 SO_UPDATE_ACCEPT_CONTEXT (iocp_win_io.hpp)
  #7 [致命] chain_fn 路径泄漏 iocp_operation → C1000K 256MB (worker_meta.cpp)

============================================================
  CHAIN CO_AWAIT 优化
============================================================

  io_uring 路径: 移植 co_context 的 lazy_link_io 模式
    - 零 move、零 refresh_user_data
    - IOSQE_IO_LINK 内核级 SQE 链接

  epoll/IOCP 路径: per-type CRTP 类型化分发
    - win_chain_base / epoll_chain_base 存储类型化分发函数
    - task_info 从 chain_fn+chain_ctx 简化为 chain_target
    - 编译器在 CRTP 实例化时已知完整类型

============================================================
