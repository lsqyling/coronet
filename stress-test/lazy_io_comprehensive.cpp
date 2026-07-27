/**
 * lazy_io_comprehensive.cpp — async_io API coverage + stress (RAII socket)
 *
 * coronet::tcp_socket 独占所有权（move-only），通过 move 传入协程。
 * 参数传递（非 lambda 捕获），避免 GCC 协程帧 bug。
 *
 * 跨平台：Windows 使用 _open/_lseeki64，Linux 使用 open/lseek。
 */

#include <coronet/all.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// ============================================================
// Cross-platform file I/O compatibility layer
// ============================================================
#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
  #include <sys/stat.h>

  #define coro_open   _open
  #define coro_close  _close
  #define coro_lseek  _lseeki64
  #define coro_unlink _unlink

  #define CORO_O_RDONLY (_O_RDONLY | _O_BINARY)
  #define CORO_O_RDWR   (_O_RDWR | _O_BINARY)
  #define CORO_O_CREAT  _O_CREAT
  #define CORO_O_TRUNC  _O_TRUNC
  #define CORO_SEEK_SET SEEK_SET

  static std::string coro_tmpdir() {
      const char* d = std::getenv("TEMP");
      return d ? std::string(d) : std::string(".");
  }
#else
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>

  #define coro_open   ::open
  #define coro_close  ::close
  #define coro_lseek  ::lseek
  #define coro_unlink ::unlink

  #define CORO_O_RDONLY O_RDONLY
  #define CORO_O_RDWR   O_RDWR
  #define CORO_O_CREAT  O_CREAT
  #define CORO_O_TRUNC  O_TRUNC
  #define CORO_SEEK_SET SEEK_SET

  static std::string coro_tmpdir() { return "/tmp"; }
#endif

using namespace coronet;
using namespace std::chrono_literals;

#define CHK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); std::abort(); \
} } while(0)

// ============================================================
// Coroutine helper functions (replacing coroutine lambdas)
// ============================================================

/// 超时后停止事件循环
static task<> stopper(io_context& ctx, int sec) {
    co_await async::timeout(std::chrono::seconds(sec));
    printf("  [TIMEOUT %ds]\n", sec);
    ctx.can_stop();
}

// ============================================================
// 1. send/recv — RAII socket + acceptor
// ============================================================

/// 服务端：accept + echo
task<> server_echo(coronet::tcp_acceptor ac) {
    int fd = co_await ac.accept();
    CHK(fd >= 0);
    coronet::tcp_socket sock{fd};
    char buf[4096];
    int nr = co_await sock.recv({buf, 4096});
    CHK(nr == 4096);
    int ns = co_await sock.send({buf, (size_t)nr});
    CHK(ns == nr);
    printf("  server: echoed %d bytes\n", ns);
    co_await sock.close();
}

/// 客户端：connect + send + recv echo
task<> client_echo(coronet::tcp_socket cli) {
    co_await cli.connect(inet_address{"127.0.0.1", 19510});
    char buf[4096];
    memset(buf, 'A', 4096);
    int ns = co_await cli.send({buf, 4096});
    CHK(ns == 4096);
    char rbuf[4096];
    int nr = co_await cli.recv({rbuf, 4096});
    CHK(nr == 4096);
    CHK(memcmp(buf, rbuf, 4096) == 0);
    printf("  client: echo verified %d bytes\n", nr);
    co_await cli.close();
}

static void phase_send_recv() {
    printf("=== Phase 1: send/recv (RAII socket) ===\n");
    io_context ctx;

    coronet::tcp_acceptor ac{inet_address{19510}};
    coronet::tcp_socket cli = coronet::tcp_socket::create_tcp(AF_INET);

    ctx.co_spawn(server_echo(std::move(ac)));
    ctx.co_spawn(client_echo(std::move(cli)));

    ctx.co_spawn(stopper(ctx, 5));
    ctx.start(); ctx.join();
    printf("[send/recv] PASSED\n");
}

// ============================================================
// 2. chained co_await (operator&&) — RAII socket
// ============================================================

/// 服务端：accept + chain echo+reply
task<> server_chain(coronet::tcp_acceptor ac) {
    int fd = co_await ac.accept();
    CHK(fd >= 0);
    coronet::tcp_socket sock{fd};
    char buf[1024];
    CHK(co_await sock.recv({buf, 1024}) == 1024);
    // chain: send echo && recv reply
    char reply[1024];
    int nr = co_await (sock.send({buf, 1024})
                    && sock.recv({reply, 1024}));
    CHK(nr == 1024);
    CHK(reply[0] == 'Y');
    printf("  server: chain echo+reply ok\n");
    co_await sock.close();
}

/// 客户端：connect + chain recv+send
task<> client_chain(coronet::tcp_socket cli) {
    co_await cli.connect(inet_address{"127.0.0.1", 19511});
    char req[1024]; memset(req, 'X', 1024);
    CHK(co_await cli.send({req, 1024}) == 1024);
    // chain: recv echo && send reply
    char echo[1024], reply[1024];
    memset(reply, 'Y', 1024);
    int nr = co_await (cli.recv({echo, 1024})
                    && cli.send({reply, 1024}));
    CHK(nr == 1024);
    CHK(memcmp(echo, req, 1024) == 0);
    printf("  client: chain verified\n");
    co_await cli.close();
}

static void phase_chain() {
    printf("=== Phase 2: chain co_await (RAII socket) ===\n");
    io_context ctx;

    coronet::tcp_acceptor ac{inet_address{19511}};
    coronet::tcp_socket cli = coronet::tcp_socket::create_tcp(AF_INET);

    ctx.co_spawn(server_chain(std::move(ac)));
    ctx.co_spawn(client_chain(std::move(cli)));

    ctx.co_spawn(stopper(ctx, 5));
    ctx.start(); ctx.join();
    printf("[chain] PASSED\n");
}

// ============================================================
// 3. shutdown — RAII socket half-close
// ============================================================

/// 服务端：accept + recv + EOF
task<> server_shutdown(coronet::tcp_acceptor ac) {
    int fd = co_await ac.accept();
    CHK(fd >= 0);
    coronet::tcp_socket sock{fd};
    char buf[256];
    CHK(co_await sock.recv({buf, 256}) == 256);
    CHK(co_await sock.recv({buf, 256}) == 0);  // EOF after SHUT_WR
    printf("  server: EOF ok\n");
    co_await sock.close();
}

/// 客户端：connect + send + shutdown + EOF
task<> client_shutdown(coronet::tcp_socket cli) {
    co_await cli.connect(inet_address{"127.0.0.1", 19512});
    char buf[256]; memset(buf, 'Z', 256);
    co_await cli.send({buf, 256});
    co_await cli.shutdown_write();
    int nr = co_await cli.recv({buf, 256});
    CHK(nr == 0);
    printf("  client: shutdown+EOF ok\n");
    co_await cli.close();
}

static void phase_shutdown() {
    printf("=== Phase 3: shutdown (RAII socket) ===\n");
    io_context ctx;

    coronet::tcp_acceptor ac{inet_address{19512}};
    coronet::tcp_socket cli = coronet::tcp_socket::create_tcp(AF_INET);

    ctx.co_spawn(server_shutdown(std::move(ac)));
    ctx.co_spawn(client_shutdown(std::move(cli)));

    ctx.co_spawn(stopper(ctx, 5));
    ctx.start(); ctx.join();
    printf("[shutdown] PASSED\n");
}

// ============================================================
// 4. File I/O — 2MB
// ============================================================

task<> phase_file_io_coro(io_context* c) {
    constexpr size_t SZ = 2 * 1024 * 1024;
    std::string fpath = coro_tmpdir() + "/coronet_fio.dat";
    int fd = coro_open(fpath.c_str(), CORO_O_RDWR | CORO_O_CREAT | CORO_O_TRUNC, 0644);
    CHK(fd >= 0);
    auto* wbuf = new char[SZ];
    for (size_t i = 0; i < SZ; ++i) wbuf[i] = (char)(i & 0xFF);

    int nw = co_await async::write(fd, {wbuf, SZ});
    CHK(nw == (int)SZ);
    printf("  wrote 2MB\n");

    coro_lseek(fd, 0, CORO_SEEK_SET);
    auto* rbuf = new char[SZ];
    int nr = co_await async::read(fd, {rbuf, SZ});
    CHK(nr == (int)SZ);
    CHK(memcmp(wbuf, rbuf, SZ) == 0);
    printf("  read+verify OK\n");

    co_await async::write(fd, {"OFFSET_OK", 9}, 0);
    char ro[16] = {};
    CHK(co_await async::read(fd, {ro, 9}, 0) == 9);
    CHK(strncmp(ro, "OFFSET_OK", 9) == 0);

    delete[] wbuf; delete[] rbuf;
    coro_close(fd); coro_unlink(fpath.c_str());
    c->can_stop();
}

static void phase_file_io() {
    printf("=== Phase 4: file I/O (2MB) ===\n");
    io_context ctx;

    ctx.co_spawn(phase_file_io_coro(&ctx));
    ctx.co_spawn(stopper(ctx, 30));

    ctx.start(); ctx.join();
    printf("[file I/O] PASSED\n");
}

// ============================================================
// 5. Large file I/O — 10MB
// ============================================================

task<> phase_large_file_coro(io_context* c) {
    constexpr size_t CHUNK = 1024 * 1024;
    constexpr int CHUNKS = 10;
    std::string fpath = coro_tmpdir() + "/coronet_big.dat";
    int fd = coro_open(fpath.c_str(), CORO_O_RDWR | CORO_O_CREAT | CORO_O_TRUNC, 0644);
    CHK(fd >= 0);
    auto* chunk = new char[CHUNK];
    memset(chunk, 0xAB, CHUNK);
    for (int i = 0; i < CHUNKS; ++i)
        CHK(co_await async::write(fd, {chunk, CHUNK}) == (int)CHUNK);
    printf("  wrote 10MB\n");

    coro_lseek(fd, 0, CORO_SEEK_SET);
    auto* vfy = new char[CHUNK];
    for (int i = 0; i < CHUNKS; ++i) {
        CHK(co_await async::read(fd, {vfy, CHUNK}) == (int)CHUNK);
        CHK(vfy[0] == (char)0xAB && vfy[CHUNK-1] == (char)0xAB);
    }
    printf("  verified 10MB\n");

    delete[] chunk; delete[] vfy;
    coro_close(fd); coro_unlink(fpath.c_str());
    c->can_stop();
}

static void phase_large_file() {
    printf("=== Phase 5: large file I/O (10MB) ===\n");
    io_context ctx;

    ctx.co_spawn(phase_large_file_coro(&ctx));
    ctx.co_spawn(stopper(ctx, 60));

    ctx.start(); ctx.join();
    printf("[large file I/O] PASSED\n");
}

// ============================================================
// 6. timeout + yield
// ============================================================

task<> phase_timeout_yield_coro(io_context* c) {
    auto t0 = std::chrono::steady_clock::now();
    co_await async::timeout(100ms);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    CHK(ms >= 80 && ms <= 200);
    printf("  timeout 100ms OK (actual=%lldms)\n", (long long)ms);

    for (int i = 0; i < 10; ++i) co_await async::yield();
    c->can_stop();
}

static void phase_timeout_yield() {
    printf("=== Phase 6: timeout/yield ===\n");
    io_context ctx;

    ctx.co_spawn(phase_timeout_yield_coro(&ctx));
    ctx.co_spawn(stopper(ctx, 5));

    ctx.start(); ctx.join();
    printf("[timeout/yield] PASSED\n");
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    phase_send_recv();
    phase_chain();
    phase_shutdown();
    phase_file_io();
    phase_large_file();
    phase_timeout_yield();
    printf("\n=== ALL LAZY_IO TESTS PASSED ===\n");
    return 0;
}
