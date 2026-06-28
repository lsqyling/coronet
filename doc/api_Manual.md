# coronet API Manual

## 1. io_context — Event Loop

```cpp
#include <coronet/io_context.hpp>
```

The core event loop. Each instance owns a platform proactor + worker thread. Multiple instances enable multi-threaded I/O.

```cpp
coronet::io_context ctx;

// Spawn a coroutine (returns immediately, runs on ctx's thread)
ctx.co_spawn(my_task());

// Start the event loop (creates a background thread)
ctx.start();

// Signal stop and wake the event loop
ctx.can_stop();

// Wait for the event loop thread to exit
ctx.join();
```

**Free functions:**

```cpp
// Spawn on the current thread's io_context
void co_spawn(task<void>&& entrance) noexcept;

// Get the current thread's io_context
io_context& this_io_context() noexcept;
```

**Lifecycle:**
```
co_spawn(task)     → push to SPSC ring
ctx.start()        → create thread → run() loop
  loop: drain → work → submit → complete
ctx.can_stop()     → set will_stop_ flag + wakeup proactor
ctx.join()         → wait for thread exit
```

**Multi-threading:** Create N `io_context` instances, spawn tasks across them, start all, join all.

---

## 2. task<T> — Lazy Coroutine Task

```cpp
#include <coronet/task.hpp>
```

Move-only, unique-ownership coroutine. Execution is lazy — the body runs only when `co_await`ed.

```cpp
task<int> compute() {
    co_return 42;
}

task<void> caller() {
    int v = co_await compute();  // compute starts and runs here
    // ...
}
```

**Key traits:**
- `task<T>` — returns a value via co_return
- `task<void>` — no return value
- `task<T&>` — returns a reference
- `initial_suspend()` → `suspend_always` (lazy start)
- `final_suspend()` → returns parent handle (inline chain)
- `detach()` → fire-and-forget (task<void> only)
- Copy disabled, move enabled

---

## 3. async_io — Async I/O Factory

```cpp
#include <coronet/async_io.hpp>
```

All functions are in `coronet::async::` namespace. Each returns an awaitable.

### Socket I/O

```cpp
// Receive data into span
auto n = co_await async::recv(fd, buf);             // → int
auto n = co_await async::recv(fd, buf, flags);      // with flags

// Send data from span
auto n = co_await async::send(fd, buf);             // → int
auto n = co_await async::send(fd, buf, flags);

// Accept connection
auto fd = co_await async::accept(listen_fd);        // → int
auto fd = co_await async::accept(listen_fd, &addr, &addrlen, flags);

// Connect
auto res = co_await async::connect(fd, &addr, addrlen);  // → int

// Close
co_await async::close(fd);

// Shutdown
co_await async::shutdown(fd, SHUT_RDWR);  // Linux: SHUT_RD/SHUT_WR/SHUT_RDWR
                                           // Win: SD_RECEIVE/SD_SEND/SD_BOTH
```

### File I/O (Linux io_uring / Windows background thread)

```cpp
auto n = co_await async::read(fd, buf);               // → int
auto n = co_await async::read(fd, buf, file_offset);

auto n = co_await async::write(fd, buf);              // → int
auto n = co_await async::write(fd, buf, file_offset);
```

### Control

```cpp
co_await async::yield();                      // Reschedule (nop)
co_await async::timeout(std::chrono::seconds{1});  // Relative delay
co_await async::timeout_at(deadline);        // Absolute time_point
```

### Chained I/O

```cpp
// Send then recv in a single suspension — kernel-level on io_uring
int n = co_await (sock.send(pong) && sock.recv(buf));
```

---

## 4. socket — Cross-Platform RAII Socket

```cpp
#include <coronet/net.hpp>
```

```cpp
coronet::socket sock{fd};  // Takes ownership of fd/SOCKET

// Async I/O via socket
int n = co_await sock.recv(buf);
int n = co_await sock.send(buf);
int n = co_await sock.connect(addr);
co_await sock.close();
co_await sock.shutdown_write();

// Synchronous setup
sock.bind(addr).listen(SOMAXCONN);
sock.set_reuse_addr(true);
sock.set_tcp_no_delay(true);
sock.set_nonblocking();

// Info
auto local = sock.local_addr();
auto peer  = sock.peer_addr();
auto fd    = sock.native_handle();

// Factory
auto tcp = socket::create_tcp(AF_INET);
auto udp = socket::create_udp(AF_INET);

// RAII: destructor calls ::close()/closesocket() if still valid
```

---

## 5. acceptor — TCP Listener

```cpp
coronet::acceptor ac{coronet::inet_address{port}};
// bind + listen happens in constructor

int client_fd = co_await ac.accept();
```

---

## 6. inet_address

```cpp
// Port only
coronet::inet_address addr{8080};

// Hostname + port (DNS resolve)
coronet::inet_address addr;
inet_address::resolve("example.com", 80, addr);

// Raw sockaddr
coronet::inet_address addr{&sa, sizeof(sa)};

// Family
auto family = addr.family();  // AF_INET or AF_INET6
auto len    = addr.length();  // sizeof(sockaddr_in) or sockaddr_in6
```

---

## 7. generator<T> — P2502R2 std::generator

```cpp
#include <coronet/generator.hpp>

coronet::generator<int> iota(int start) {
    while (true) {
        co_yield start++;
    }
}

// Use with ranges
for (auto x : iota(1) | std::views::take(5)) {
    std::cout << x << " ";  // 1 2 3 4 5
}
```

Supports: value, reference, const reference types, custom allocators, `elements_of()`.

---

## 8. shared_task<T> — Multi-Waiter

```cpp
#include <coronet/shared_task.hpp>

shared_task<int> compute() { co_return 42; }

auto st = compute();        // Starts executing
int a = co_await st;        // First consumer
int b = co_await st;        // Second consumer (same result)
// Reference-counted — destroyed when last reference gone
```

---

## 9. mutex — Coroutine Mutex

```cpp
#include <coronet/co/mutex.hpp>

coronet::mutex mtx;
int counter = 0;

task<> increment() {
    auto guard = co_await mtx.lock_guard();  // RAII
    ++counter;                                // protected
}   // auto-unlock on scope exit

// Manual lock/unlock
co_await mtx.lock();
mtx.unlock();
```

CAS-based fast path for uncontended lock. Linked-list waiter queue for contended case. Cross-thread safe.

---

## 10. condition_variable

```cpp
coronet::condition_variable cv;
coronet::mutex mtx;
bool ready = false;

task<> waiter() {
    auto lk = co_await mtx.lock_guard();
    co_await cv.wait(mtx, [&] { return ready; });
    // proceed with ready == true
}

task<> notifier() {
    {
        auto lk = co_await mtx.lock_guard();
        ready = true;
    }
    cv.notify_one();   // or cv.notify_all()
}
```

---

## 11. counting_semaphore

```cpp
coronet::counting_semaphore sem{3};  // max 3 concurrent

task<> worker(int id) {
    co_await sem.acquire();   // wait for slot
    // ... up to 3 workers here
    sem.release();            // free slot
}
```

---

## 12. channel<T, Capacity> — CSP Channel

```cpp
coronet::channel<std::string, 8> ch;

// Producer
task<> produce() {
    co_await ch.release("hello");
}

// Consumer
task<> consume() {
    std::string msg = co_await ch.acquire();
}
```

**Capacity modes:**
- `channel<T>` (default, capacity=0) — rendezvous (producer blocks until consumer ready)
- `channel<T, 1>` — single-slot buffer
- `channel<T, N>` — N-slot buffer

---

## 13. when_all / when_any / when_some — Combinators

```cpp
#include <coronet/co/when_all.hpp>

// Wait for ALL tasks, get results as tuple (void filtered out)
auto [r0, r1] = co_await all(f0(), f1(), f2());

// Wait for FIRST task to complete
auto [idx, var] = co_await any(f0(), f1(), f2());

// Wait for N tasks to complete
auto results = co_await some(2, f0(), f1(), f2());

// std::visit helper
std::visit(overload{
    [](int x)            { /* handle int */ },
    [](const std::string& s) { /* handle string */ },
    [](std::monostate)   { /* handle void */ },
}, var);
```

---

## 14. defer — RAII Scope Guard

```cpp
#include <coronet/utility/defer.hpp>

task<> example(int fd) {
    defer _{ [fd] { ::close(fd); } };  // always runs on scope exit
    co_await async::write(fd, buf);
    // fd closed here even if exception
}
```

---

## API Quick Reference

| Category | Header | Key Types/Functions |
|----------|--------|---------------------|
| **Core** | `coronet/task.hpp` | `task<T>`, `task<void>`, `task<T&>` |
|  | `coronet/io_context.hpp` | `io_context`, `co_spawn()`, `this_io_context()` |
|  | `coronet/shared_task.hpp` | `shared_task<T>` |
|  | `coronet/generator.hpp` | `generator<T>` |
| **I/O** | `coronet/async_io.hpp` | `async::recv/send/accept/connect/close/timeout/yield/read/write` |
| **Net** | `coronet/net.hpp` | `socket`, `acceptor`, `inet_address` |
| **Sync** | `coronet/co/mutex.hpp` | `mutex`, `lock_guard` |
|  | `coronet/co/condition_variable.hpp` | `condition_variable` |
|  | `coronet/co/semaphore.hpp` | `counting_semaphore` |
|  | `coronet/co/channel.hpp` | `channel<T, N>` |
| **Compose** | `coronet/co/when_all.hpp` | `all()`, `any()`, `some()`, `overload{}` |
| **Utility** | `coronet/utility/defer.hpp` | `defer{}` |
| **All-in-one** | `coronet/all.hpp` | includes all above |
