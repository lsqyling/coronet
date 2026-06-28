# CLAUDE.md

Coronet — cross-platform C++20 coroutine async I/O library (io_uring + IOCP).

## Build

```bash
# Linux (GCC)
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release

# Linux (Clang)
cmake -S . -B build-clang -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake --build build-clang

# Windows (MSVC)
cmake -B build && cmake --build build --config Release

# Enable examples/benchmarks
cmake -S . -B build-release -DCORONET_BUILD_EXAMPLES=ON -DCORONET_BUILD_BENCHMARKS=ON
```

## Test

```bash
# Build & run all tests
cmake --build build-release --target task_gtest generator_gtest proactor_gtest \
    channel_gtest shared_task_gtest ft_task coro_lifetime generator_test \
    move_shared_task stress_test

# Build & run all examples
cmake --build build-release --target example_iota example_timer example_mutex \
    example_channel example_cv_notify_all example_sem example_echo_server \
    example_echo_server_MT example_httpd example_httpd_MT example_netcat \
    example_when_all example_when_any example_when_some example_timer_accuracy

# Smoke tests
bash doc/linux/smoke_all.sh          # Linux
powershell doc/win/smoke.ps1          # Windows
```

## Architecture

```
include/coronet/
  task.hpp              — lazy unique-ownership coroutine task<T/void/T&>
  generator.hpp         — P2502R2 std::generator reference impl
  shared_task.hpp       — ref-counted multi-waiter task
  io_context.hpp        — event loop / scheduler (compile-time platform select)
  async_io.hpp          — cross-platform I/O factory (recv/send/accept/connect/...)
  co/
    channel.hpp         — CSP channel (buffered/unbuffered/rendezvous)
    mutex.hpp           — coroutine mutex (spinlock + linked-list waiters)
    condition_variable.hpp
    semaphore.hpp       — counting_semaphore
    when_all.hpp        — all/any/some coroutine combinators
  net/
    socket.hpp          — cross-platform socket (int fd on Linux, SOCKET on Win)
    acceptor.hpp        — TCP acceptor
    inet_address.hpp    — IPv4/IPv6 address + DNS resolve
  platform/
    platform.hpp        — platform detection + completion_info
    proactor.hpp        — C++20 concepts (operation_concept, proactor_concept)
    io_uring/           — Linux io_uring backend
    iocp/               — Windows IOCP backend
  detail/
    worker_meta.hpp     — per-worker state (SPSC ring, cross-thread queue)
    spsc_cursor.hpp     — lock-free SPSC cursor
    task_info.hpp       — per-I/O metadata (handle, result, chain)
    chained_awaiter.hpp — operator&& for co_await (send && recv)
  utility/
    defer.hpp           — RAII defer (co_context-aligned)
  log/log.hpp           — compile-time-level logging
lib/coronet/
  io_context.cpp        — event loop (run/start/join/co_spawn)
  detail/worker_meta.cpp
  platform/{io_uring,iocp}/*.cpp
```

## Key Design Decisions

- **Static polymorphism**: `#ifdef` selects concrete proactor at compile time. No virtual dispatch.
- **Platform factory**: `async_io.hpp` calls `detail::platform_io::make_*()` — zero `#ifdef` in function bodies.
- **SPSC ring**: lock-free `forward_task/schedule` for same-thread coroutine resumption.
- **Cross-thread co_spawn**: mutex queue + `eventfd` (Linux) / `PostQueuedCompletionStatus` (Win).
- **reap_swap**: heap-allocated `std::vector` (131KB; avoids stack overflow with N≥8 io_contexts).
- **Chained co_await**: `operator&&` → kernel `IOSQE_IO_LINK` (io_uring) or user `chain_fn` (IOCP).

## Platform Notes

- **Windows**: default thread stack = 1MB. `io_context ctx[N]` with N≥8 overflows.
  Use `N≤6` or increase stack size (`/STACK:2097152` linker flag).
- **Linux**: default stack = 8MB. No such limitation.

## Scripts

| Location | Content |
|----------|---------|
| `script/linux/` | Linux benchmark + smoke test + build scripts |
| `script/win/` | Windows benchmark + smoke test + build scripts |
| `doc/aio_PR.md` | Complete performance report |
