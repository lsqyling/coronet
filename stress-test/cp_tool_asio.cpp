/**
 * cp_tool_asio.cpp — asio 协程版文件拷贝压测（对照实验用）
 *
 * 与 cp_tool.cpp 严格镜像：
 *   - 相同 CLI：[src] [dst] [--chunk MB] [--no-verify] [--timeout SEC]
 *   - 相同 copy 结构：双缓冲、严格串行（read 完成后才发 write）、offset-based
 *   - 相同 verify：逐 chunk FNV-1a64 hash 比对
 *   - 相同输出格式（copy/verify 行直接可比）
 *
 * I/O 后端：asio::random_access_file 的 async_read_some_at / async_write_some_at
 * （Linux + liburing 存在时走 asio io_uring 后端 —— 与 coronet 同一内核机制）
 *
 * 用途：在相同路径/相同窗口下与 coronet cp_tool 对照，
 *       判定吞吐差异来自库实现还是 copy 循环结构 / 9P 链路。
 */

#include <asio.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using asio::random_access_file;

constexpr size_t kDefaultChunk = 4 * 1024 * 1024;

static std::string g_src_path;
static std::string g_dst_path;
static size_t g_chunk_size = kDefaultChunk;
static bool g_verify = true;
static int g_timeout_sec = 600;

// ---- FNV-1a 64（与 cp_tool.cpp 相同）----
static uint64_t hash_chunk(const char* data, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= (unsigned char)data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// ---- copy coroutine（镜像 cp_tool copy_coro：双缓冲、串行 read→write）----
asio::awaitable<void> copy_coro(random_access_file& src, random_access_file& dst,
                                uint64_t file_size, uint64_t* copy_ms, int* chunks) {
    std::vector<char> buf0(g_chunk_size);
    std::vector<char> buf1(g_chunk_size);
    char* bufs[2] = {buf0.data(), buf1.data()};

    uint64_t offset = 0;
    int cur = 0;
    bool has_pending_write = false;
    int pending_write_len = 0;
    int pending_write_buf = 0;
    uint64_t pending_write_offset = 0;

    auto t0 = std::chrono::steady_clock::now();

    try {
        while (offset < file_size) {
            size_t to_read = (offset + g_chunk_size <= file_size)
                             ? g_chunk_size
                             : (size_t)(file_size - offset);

            std::size_t nr = co_await src.async_read_some_at(
                offset, asio::buffer(bufs[cur], to_read), asio::use_awaitable);

            if (has_pending_write) {
                std::size_t nw = co_await dst.async_write_some_at(
                    pending_write_offset,
                    asio::buffer(bufs[pending_write_buf], (size_t)pending_write_len),
                    asio::use_awaitable);
                if (nw != (size_t)pending_write_len) {
                    printf("FAIL: short write %zu/%d\n", nw, pending_write_len);
                    std::abort();
                }
                (*chunks)++;
            }

            pending_write_buf = cur;
            pending_write_len = (int)nr;
            pending_write_offset = offset;
            has_pending_write = true;

            offset += nr;
            cur = 1 - cur;
        }

        if (has_pending_write) {
            std::size_t nw = co_await dst.async_write_some_at(
                pending_write_offset,
                asio::buffer(bufs[pending_write_buf], (size_t)pending_write_len),
                asio::use_awaitable);
            if (nw != (size_t)pending_write_len) {
                printf("FAIL: short write %zu/%d\n", nw, pending_write_len);
                std::abort();
            }
            (*chunks)++;
        }
    } catch (const asio::system_error& e) {
        printf("FAIL: copy error: %s\n", e.what());
        std::abort();
    }

    *copy_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    co_return;
}

// ---- verify coroutine（镜像 cp_tool verify_coro）----
asio::awaitable<void> verify_coro(random_access_file& src, random_access_file& dst,
                                  uint64_t file_size, uint64_t* verify_ms,
                                  int* chunks_checked, bool* verify_ok) {
    std::vector<char> sbuf(g_chunk_size);
    std::vector<char> dbuf(g_chunk_size);
    auto t0 = std::chrono::steady_clock::now();

    uint64_t offset = 0;
    int mismatches = 0;

    try {
        while (offset < file_size) {
            size_t to_read = (offset + g_chunk_size <= file_size)
                             ? g_chunk_size
                             : (size_t)(file_size - offset);

            std::size_t nr_s = co_await src.async_read_some_at(
                offset, asio::buffer(sbuf.data(), to_read), asio::use_awaitable);
            std::size_t nr_d = co_await dst.async_read_some_at(
                offset, asio::buffer(dbuf.data(), to_read), asio::use_awaitable);

            uint64_t h_src = hash_chunk(sbuf.data(), nr_s);
            uint64_t h_dst = hash_chunk(dbuf.data(), nr_d);

            if (h_src != h_dst) {
                mismatches++;
                printf("  MISMATCH at offset %llu: src_hash=%016llx dst_hash=%016llx\n",
                       (unsigned long long)offset,
                       (unsigned long long)h_src,
                       (unsigned long long)h_dst);
            }

            (*chunks_checked)++;
            offset += nr_s;
        }
    } catch (const asio::system_error& e) {
        printf("FAIL: verify error: %s\n", e.what());
        std::abort();
    }

    *verify_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    *verify_ok = (mismatches == 0);
    co_return;
}

// ---- 主协程：串链 copy → verify（单次 run()，asio 标准模式）----
// 命名协程函数（非协程 lambda —— asio 官方不推荐协程 lambda，捕获/生命周期
// 语义复杂易错；本项目 cp_tool 也约定"协程函数（非 lambda），参数值传递"）。
// 注意：不能分两次 io.run() —— scheduler::run() 在 outstanding_work_==0 时
// 会自停（stop()），后续 run() 直接返回不再处理任何 handler。
// 超时由外层 shell timeout 兜底（--timeout 保留解析以兼容 cp_tool CLI）。
asio::awaitable<void> copy_and_verify(random_access_file& src, random_access_file& dst,
                                      uint64_t file_size, bool do_verify,
                                      uint64_t* copy_ms, int* chunks_copied,
                                      uint64_t* verify_ms, int* chunks_checked,
                                      bool* verify_ok) {
    printf("--- Phase 1: Async Copy ---\n");
    co_await copy_coro(src, dst, file_size, copy_ms, chunks_copied);
    if (do_verify) {
        printf("\n--- Phase 2: Verify ---\n");
        co_await verify_coro(src, dst, file_size, verify_ms, chunks_checked, verify_ok);
    }
}

// ---- CLI（镜像 cp_tool parse_args）----
static void print_usage(const char* prog) {
    printf("Usage: %s [src] [dst] [options]\n", prog);
    printf("  --chunk MB   Chunk size in MB (default: 4)\n");
    printf("  --no-verify  Skip verification after copy\n");
    printf("  --timeout S  Timeout in seconds (default: 600)\n");
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::string fname = std::filesystem::path(argv[0]).filename().string();

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--chunk" && i + 1 < argc) {
            int mb = std::atoi(argv[++i]);
            if (mb > 0) g_chunk_size = (size_t)mb * 1024 * 1024;
        } else if (arg == "--no-verify") {
            g_verify = false;
        } else if (arg == "--timeout" && i + 1 < argc) {
            g_timeout_sec = std::atoi(argv[++i]);
        } else if (arg.substr(0, 2) == "--") {
            print_usage(argv[0]);
            return 1;
        } else if (g_src_path.empty()) {
            g_src_path = argv[i];
        } else if (g_dst_path.empty()) {
            g_dst_path = argv[i];
        }
    }
    if (g_src_path.empty() || g_dst_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    printf("=== asio cp_tool — coroutine file copy ===\n");
    printf("  src:    %s\n", g_src_path.c_str());
    printf("  dst:    %s\n", g_dst_path.c_str());
    printf("  chunk:  %zu MB\n", g_chunk_size / (1024 * 1024));
    printf("  verify: %s\n", g_verify ? "YES" : "NO");
    printf("  timeout: %ds\n", g_timeout_sec);
    printf("\n");

    uint64_t file_size = (uint64_t)std::filesystem::file_size(g_src_path);
    printf("  source size: %llu bytes (%.2f GB)\n\n",
           (unsigned long long)file_size, file_size / (1024.0 * 1024.0 * 1024.0));

    asio::io_context io;

    random_access_file src(io);
    std::error_code ec;
    src.open(g_src_path, random_access_file::read_only, ec);
    if (ec) { printf("FAIL: open src: %s\n", ec.message().c_str()); return 1; }

    random_access_file dst(io);
    dst.open(g_dst_path, random_access_file::read_write |
                         random_access_file::create | random_access_file::truncate, ec);
    if (ec) { printf("FAIL: open dst: %s\n", ec.message().c_str()); return 1; }

    uint64_t copy_ms = 0;
    int chunks_copied = 0;
    uint64_t verify_ms = 0;
    int chunks_checked = 0;
    bool verify_ok = false;

    asio::co_spawn(io, copy_and_verify(src, dst, file_size, g_verify,
                                       &copy_ms, &chunks_copied,
                                       &verify_ms, &chunks_checked, &verify_ok),
                   asio::detached);
    io.run();

    double gb = file_size / (1024.0 * 1024.0 * 1024.0);
    double mb = file_size / (1024.0 * 1024.0);
    double copy_mbps = copy_ms > 0 ? mb / (copy_ms / 1000.0) : 0.0;
    printf("  copy: %llu bytes (%.2f GB) in %llums (%.0f MB/s), %d chunks\n",
           (unsigned long long)file_size, gb,
           (unsigned long long)copy_ms, copy_mbps, chunks_copied);
    if (g_verify) {
        double verify_mbps = verify_ms > 0 ? mb / (verify_ms / 1000.0) : 0.0;
        printf("  verify: %d chunks checked, %s, %llums (%.0f MB/s)\n",
               chunks_checked, verify_ok ? "OK" : "FAILED",
               (unsigned long long)verify_ms, verify_mbps);
    }

    printf("\n=== %s ===\n", (!g_verify || verify_ok) ? "ALL PASSED" : "FAILED");
    return (!g_verify || verify_ok) ? 0 : 1;
}
