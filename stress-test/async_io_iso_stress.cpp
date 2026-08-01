/**
 * async_io_iso_stress.cpp — 5GB ISO 极限文件 I/O 压测
 *
 * 覆盖 async_io 全部 API：
 *   read    — 顺序读、offset 随机读、大/小 chunk
 *   write   — 临时文件写出 + 读回验证
 *   chain   — read && read 链式 co_await
 *   timeout — 超时控制
 *   yield   — 协程让出
 *
 * 所有协程使用协程函数（非 lambda），参数值传递。
 * 跨平台：文件定位（获取大小/seek）使用 std::filesystem::file_size +
 *   std::fstream + seekg/seekp，消除 lseek/_lseeki64 的平台差异。
 *   文件打开/关闭仍用平台 API（获取 fd 以调用 async::read/write）。
 */

#include <coronet/all.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

// ============================================================
// Cross-platform file I/O compatibility layer
// ============================================================
// 文件打开/关闭使用平台原生 API（open/close 是获取 fd 的唯一途径）;
// 文件定位（获取大小、seek）统一使用 std::filesystem + std::fstream，
//   消除 lseek / _lseeki64 的平台差异。
//
// File open/close: platform-native (only way to get an fd for async I/O).
// File positioning & size: std::filesystem + std::fstream (seekg/tellg),
//   eliminating lseek / _lseeki64 platform differences.
#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
  #include <sys/stat.h>

  #define coro_open   _open
  #define coro_close  _close
  #define coro_unlink _unlink

  #define CORO_O_RDONLY (_O_RDONLY | _O_BINARY)
  #define CORO_O_RDWR   (_O_RDWR | _O_BINARY)
  #define CORO_O_CREAT  _O_CREAT
  #define CORO_O_TRUNC  _O_TRUNC
  #define CORO_O_WRONLY (_O_WRONLY | _O_BINARY)

  static const char* ISO_PATH = "D:/dev/Downloads/windows_10_professional_x64_2026.iso";
#else
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>

  #define coro_open   ::open
  #define coro_close  ::close
  #define coro_unlink ::unlink

  #define CORO_O_RDONLY O_RDONLY
  #define CORO_O_RDWR   O_RDWR
  #define CORO_O_CREAT  O_CREAT
  #define CORO_O_TRUNC  O_TRUNC
  #define CORO_O_WRONLY O_WRONLY

  static const char* ISO_PATH = "/mnt/d/dev/Downloads/windows_10_professional_x64_2026.iso";
#endif

using namespace coronet;
using namespace std::chrono_literals;

#define CHK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); std::abort(); \
} } while(0)

static auto t_start = std::chrono::steady_clock::now();
static void tick(const char* msg) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start).count();
    printf("[%6lldms] %s\n", (long long)ms, msg);
    fflush(stdout);
}

// ============================================================
// Coroutine helper functions (replacing coroutine lambdas)
// ============================================================

/// 超时后停止事件循环
task<> timeout_stop(io_context* c, int sec) {
    co_await async::timeout(std::chrono::seconds(sec));
    printf("  [TIMEOUT %ds]\n", sec);
    c->can_stop();
}

// ============================================================
// Phase 1: 顺序读 5GB — 1MB chunks
// ============================================================

task<> phase1_coro(int fd, uint64_t size, io_context* c) {
    constexpr size_t CHUNK = 1024 * 1024;  // 1MB
    auto* buf = new char[CHUNK];
    uint64_t total = 0;
    size_t first_chunk_hash = 0, mid_chunk_hash = 0, last_chunk_hash = 0;
    uint64_t mid_point = size / 2;

    tick("sequential read start");
    auto t0 = std::chrono::steady_clock::now();

    while (total < size) {
        size_t to_read = (total + CHUNK <= size) ? CHUNK : (size_t)(size - total);
        int nr = co_await async::read(fd, {buf, to_read});
        if (nr <= 0) break;

        if (total == 0) {
            for (size_t i = 0; i < (size_t)nr; ++i)
                first_chunk_hash = first_chunk_hash * 31 + (unsigned char)buf[i];
        }
        if (total <= mid_point && total + nr > mid_point) {
            for (int i = 0; i < nr; ++i)
                mid_chunk_hash = mid_chunk_hash * 31 + (unsigned char)buf[i];
        }

        total += nr;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    // Read last chunk using offset parameter (replaces coro_lseek + read)
    uint64_t last_offset = size > CHUNK ? size - CHUNK : 0;
    int nr = co_await async::read(fd, {buf, CHUNK}, last_offset);
    for (int i = 0; i < nr; ++i)
        last_chunk_hash = last_chunk_hash * 31 + (unsigned char)buf[i];

    double gb = total / (1024.0 * 1024.0 * 1024.0);
    double mbps = elapsed > 0 ? (total / (1024.0 * 1024.0)) / (elapsed / 1000.0) : 0;
    printf("  read %llu bytes (%.2f GB) in %lldms\n",
           (unsigned long long)total, gb, (long long)elapsed);
    printf("  throughput: %.0f MB/s\n", mbps);
    printf("  hashes: first=%zu mid=%zu last=%zu\n",
           first_chunk_hash, mid_chunk_hash, last_chunk_hash);
    CHK(total == size);
    CHK(first_chunk_hash != 0);
    CHK(last_chunk_hash != 0);

    delete[] buf;
    tick("sequential read complete");
    c->can_stop();
}

static void phase_sequential_read() {
    printf("\n=== Phase 1: Sequential Read 5GB (1MB chunks) ===\n");

    // 使用 std::filesystem::file_size 获取文件大小（替代 lseek SEEK_END）
    // Use std::filesystem::file_size to get file size (replaces lseek SEEK_END)
    uint64_t file_size = std::filesystem::file_size(ISO_PATH);

    int fd = coro_open(ISO_PATH, CORO_O_RDONLY);
    CHK(fd >= 0);
    printf("  file_size=%llu bytes (%.1f GB)\n",
           (unsigned long long)file_size, file_size / (1024.0 * 1024.0 * 1024.0));

    io_context ctx;

    ctx.co_spawn(phase1_coro(fd, file_size, &ctx));
    ctx.co_spawn(timeout_stop(&ctx, 120));

    ctx.start(); ctx.join();
    coro_close(fd);
    printf("[Phase 1] PASSED\n");
}

// ============================================================
// Phase 2: 随机 offset 读 — 模拟 pread
// ============================================================

task<> phase2_coro(int fd, uint64_t size, io_context* c) {
    constexpr int N = 200;
    constexpr size_t CHUNK = 65536;  // 64KB
    auto* buf = new char[CHUNK];
    int ok = 0, fail = 0;

    tick("random read start");
    auto t0 = std::chrono::steady_clock::now();

    for (int i = 0; i < N; ++i) {
        uint64_t offset = static_cast<uint64_t>(rand()) % (size - CHUNK);
        int nr = co_await async::read(fd, {buf, CHUNK}, (uint64_t)offset);
        if (nr == (int)CHUNK) {
            ok++;
        } else {
            fail++;
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    printf("  %d random reads: %d OK, %d FAIL in %lldms\n",
           N, ok, fail, (long long)elapsed);
    CHK(ok == N);
    CHK(fail == 0);

    delete[] buf;
    tick("random read complete");
    c->can_stop();
}

static void phase_random_read() {
    printf("\n=== Phase 2: Random Offset Read ===\n");

    uint64_t size = std::filesystem::file_size(ISO_PATH);
    int fd = coro_open(ISO_PATH, CORO_O_RDONLY);
    CHK(fd >= 0);

    io_context ctx;

    ctx.co_spawn(phase2_coro(fd, size, &ctx));
    ctx.co_spawn(timeout_stop(&ctx, 30));

    ctx.start(); ctx.join();
    coro_close(fd);
    printf("[Phase 2] PASSED\n");
}

// ============================================================
// Phase 3: 链式 co_await — read && read
// ============================================================

task<> phase3_coro(int fd, io_context* c) {
    constexpr size_t SZ = 4096;
    char buf1[SZ], buf2[SZ];

    // Chained co_await: read at offset 32KB, then at 1MB.
    // operator&& chains two async::read into a single co_await expression.
    // The second read's result is returned.
    int nr = co_await (async::read(fd, {buf1, SZ}, 32768)
                    && async::read(fd, {buf2, SZ}, 1024 * 1024));
    printf("  chain read: second read returned %d bytes\n", nr);
    CHK(nr == (int)SZ);

    // Verify both reads returned readable data (not all zeros).
    // ISO files are dense binary data — first/last bytes unlikely all-zero.
    bool buf1_ok = false, buf2_ok = false;
    for (size_t i = 0; i < SZ; ++i) {
        if (buf1[i] != 0) { buf1_ok = true; break; }
    }
    for (size_t i = 0; i < SZ; ++i) {
        if (buf2[i] != 0) { buf2_ok = true; break; }
    }
    CHK(buf1_ok && buf2_ok);
    printf("  chain read: both buffers contain valid data\n");
    c->can_stop();
}

static void phase_chained_read() {
    printf("\n=== Phase 3: Chained co_await (read && read) ===\n");

    int fd = coro_open(ISO_PATH, CORO_O_RDONLY);
    CHK(fd >= 0);

    io_context ctx;

    ctx.co_spawn(phase3_coro(fd, &ctx));
    ctx.co_spawn(timeout_stop(&ctx, 10));

    ctx.start(); ctx.join();
    coro_close(fd);
    printf("[Phase 3] PASSED\n");
}

// ============================================================
// Phase 4: 大文件 write → read 验证
// ============================================================

task<> phase4_coro(const char* path, const char* data, size_t sz, io_context* c) {
    int fd = coro_open(path, CORO_O_RDWR | CORO_O_CREAT | CORO_O_TRUNC, 0644);
    CHK(fd >= 0);

    // Write 100MB in 1MB chunks via async::write
    constexpr size_t CHUNK = 1024 * 1024;
    tick("write start");
    auto t0 = std::chrono::steady_clock::now();

    for (size_t off = 0; off < sz; off += CHUNK) {
        size_t remain = sz - off;
        size_t wsize = remain < CHUNK ? remain : CHUNK;
        int nw = co_await async::write(fd, {data + off, wsize});
        CHK(nw == (int)wsize);
    }

    auto t1 = std::chrono::steady_clock::now();
    auto wms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    printf("  wrote %.0f MB in %lldms (%.0f MB/s)\n",
           sz / (1024.0 * 1024.0), (long long)wms,
           wms > 0 ? (sz / (1024.0 * 1024.0)) / (wms / 1000.0) : 0);

    // Read back and verify: use async::read with explicit offset=0
    // (replaces coro_lseek + async::read).  Tests async::read offset parameter.
    auto* vfy = new char[CHUNK];
    for (size_t off = 0; off < sz; off += CHUNK) {
        size_t remain = sz - off;
        size_t rsize = remain < CHUNK ? remain : CHUNK;
        int nr = co_await async::read(fd, {vfy, rsize}, static_cast<uint64_t>(off));
        CHK(nr == (int)rsize);
        CHK(memcmp(data + off, vfy, rsize) == 0);
    }

    auto t2 = std::chrono::steady_clock::now();
    auto rms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    printf("  verified %.0f MB in %lldms (%.0f MB/s)\n",
           sz / (1024.0 * 1024.0), (long long)rms,
           rms > 0 ? (sz / (1024.0 * 1024.0)) / (rms / 1000.0) : 0);

    delete[] vfy;
    coro_close(fd);
    coro_unlink(path);
    tick("write+verify complete");
    c->can_stop();
}

static void phase_write_and_verify() {
    printf("\n=== Phase 4: Write + Read-back Verify ===\n");

    // Use std::filesystem for temp directory (replaces coro_tmpdir)
    std::string tmpfile = (std::filesystem::temp_directory_path() /
                           "coronet_iso_write_test.dat").string();
    constexpr size_t WRITE_SIZE = 100 * 1024 * 1024;  // 100MB

    // Read 100MB test data from ISO using std::ifstream + seekg
    // (replaces coro_open + coro_read + coro_close)
    auto* src_data = new char[WRITE_SIZE];
    {
        std::ifstream ifs(ISO_PATH, std::ios::binary);
        CHK(ifs.good());
        ifs.read(src_data, WRITE_SIZE);
        CHK(ifs.gcount() == (std::streamsize)WRITE_SIZE);
    }

    io_context ctx;

    ctx.co_spawn(phase4_coro(tmpfile.c_str(), src_data, WRITE_SIZE, &ctx));
    ctx.co_spawn(timeout_stop(&ctx, 60));

    ctx.start(); ctx.join();
    delete[] src_data;
    printf("[Phase 4] PASSED\n");
}

// ============================================================
// Phase 5: 混合大小 chunk — 从 512B 到 8MB
// ============================================================

task<> phase5_coro(int fd, uint64_t size, io_context* c) {
    constexpr size_t sizes[] = {512, 4096, 65536, 262144, 1048576, 4194304, 8388608};
    int ok = 0;

    for (size_t chunk : sizes) {
        auto* buf = new char[chunk];
        uint64_t offset = static_cast<uint64_t>(rand()) % (size - chunk - 1);
        int nr = co_await async::read(fd, {buf, chunk}, offset);
        if (nr == (int)chunk) ok++;

        delete[] buf;
    }

    printf("  %zu mixed-size reads: %d/%zu OK\n",
           sizeof(sizes)/sizeof(sizes[0]), ok, sizeof(sizes)/sizeof(sizes[0]));
    CHK(ok == (int)(sizeof(sizes)/sizeof(sizes[0])));
    c->can_stop();
}

static void phase_mixed_chunks() {
    printf("\n=== Phase 5: Mixed Chunk Sizes ===\n");

    uint64_t size = std::filesystem::file_size(ISO_PATH);
    int fd = coro_open(ISO_PATH, CORO_O_RDONLY);
    CHK(fd >= 0);

    io_context ctx;

    ctx.co_spawn(phase5_coro(fd, size, &ctx));
    ctx.co_spawn(timeout_stop(&ctx, 10));

    ctx.start(); ctx.join();
    coro_close(fd);
    printf("[Phase 5] PASSED\n");
}

// ============================================================
// Phase 6: timeout + yield 组合
// ============================================================

task<> phase6_coro(io_context* c) {
    // Test timeout accuracy
    auto t0 = std::chrono::steady_clock::now();
    co_await async::timeout(500ms);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    printf("  timeout 500ms: actual=%lldms\n", (long long)ms);
    CHK(ms >= 450 && ms <= 600);  // 50ms tolerance

    // Test yield — 100 iterations should be fast
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) co_await async::yield();
    auto yms = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    printf("  yield x100: %lldus\n", (long long)yms);

    // timeout_at
    auto deadline = std::chrono::steady_clock::now() + 200ms;
    co_await async::timeout_at(deadline);
    ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    CHK(ms >= 100);  // at least some time passed after 100 yields + 200ms

    c->can_stop();
}

static void phase_control_ops() {
    printf("\n=== Phase 6: Control Ops (timeout + yield) ===\n");

    io_context ctx;

    ctx.co_spawn(phase6_coro(&ctx));
    ctx.co_spawn(timeout_stop(&ctx, 10));

    ctx.start(); ctx.join();
    printf("[Phase 6] PASSED\n");
}

// ============================================================
// main
// ============================================================

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== async_io ISO Stress Test ===\n");
    printf("ISO: %s\n", ISO_PATH);

    t_start = std::chrono::steady_clock::now();

    phase_sequential_read();
    phase_random_read();
    phase_chained_read();
    phase_write_and_verify();
    phase_mixed_chunks();
    phase_control_ops();

    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start).count();
    printf("\n=== ALL PHASES PASSED (total %lldms) ===\n", (long long)total_ms);
    return 0;
}
