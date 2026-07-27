/**
 * cp_tool.cpp — 基于 async_io + io_context 的文件拷贝压测工具
 *
 * 功能：
 *   - 使用 coronet::async::read / async::write 异步 I/O 复制大文件
 *   - 集成 Sampler 采集 CPU/内存使用信息
 *   - 支持命令行参数指定源文件和目标路径
 *   - 拷贝后验证数据完整性（逐 chunk hash 比对）
 *   - 输出吞吐量、CPU%、内存等指标到 CSV
 *
 * 用法：
 *   cp_tool [src] [dst] [--chunk SIZE_MB] [--no-verify] [--timeout SEC]
 *
 * 默认：将 data/windows_10_professional_x64_2026.iso (5GB) 复制到桌面
 *
 * 设计要点：
 *   - 协程函数（非 lambda），参数值传递 — 遵循项目约定
 *   - 使用 offset-based read/write 避免文件指针竞争
 *   - double-buffering：读和写可以交错（pipeline）
 *   - Sampler 独立线程采样，不阻塞 I/O 协程
 */

#include <coronet/all.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <filesystem>
#include <thread>
#include <atomic>
#include <algorithm>

// ============================================================
// Cross-platform file I/O compatibility layer
// ============================================================
#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
  #include <sys/stat.h>

  #define coro_open   _open
  #define coro_close  _close
  #define coro_read   _read
  #define coro_lseek  _lseeki64
  #define coro_unlink _unlink

  #define CORO_O_RDONLY (_O_RDONLY | _O_BINARY)
  #define CORO_O_RDWR   (_O_RDWR | _O_BINARY)
  #define CORO_O_CREAT  _O_CREAT
  #define CORO_O_TRUNC  _O_TRUNC
  #define CORO_O_WRONLY (_O_WRONLY | _O_BINARY)
  #define CORO_SEEK_SET SEEK_SET
  #define CORO_SEEK_END SEEK_END

  using coro_off_t = __int64;

  // Windows: get desktop path via SHGetFolderPath equivalent
  #include <shlobj.h>
  #pragma comment(lib, "shell32.lib")
  static std::string get_default_desktop() {
      char path[MAX_PATH] = {};
      if (SHGetFolderPathA(nullptr, CSIDL_DESKTOP, nullptr, 0, path) == S_OK) {
          return std::string(path);
      }
      // fallback
      const char* d = std::getenv("USERPROFILE");
      if (d) return std::string(d) + "\\Desktop";
      return ".";
  }
#else
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>

  #define coro_open   ::open
  #define coro_close  ::close
  #define coro_read   ::read
  #define coro_lseek  ::lseek
  #define coro_unlink ::unlink

  #define CORO_O_RDONLY O_RDONLY
  #define CORO_O_RDWR   O_RDWR
  #define CORO_O_CREAT  O_CREAT
  #define CORO_O_TRUNC  O_TRUNC
  #define CORO_O_WRONLY O_WRONLY
  #define CORO_SEEK_SET SEEK_SET
  #define CORO_SEEK_END SEEK_END

  using coro_off_t = off_t;

  static std::string get_default_desktop() {
      const char* home = std::getenv("HOME");
      if (home) return std::string(home) + "/Desktop";
      return "/tmp";
  }
#endif

using namespace coronet;
using namespace std::chrono_literals;

#define CHK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); std::abort(); \
} } while(0)

// ============================================================
// Default paths
// ============================================================
static std::string DEFAULT_SRC = "D:/dev/workspace/yidaoyun/coronet-win/data/windows_10_professional_x64_2026.iso";
static std::string g_src_path;
static std::string g_dst_path;
static size_t g_chunk_size = 4 * 1024 * 1024;  // 4MB default
static bool g_verify = true;
static int g_timeout_sec = 600;  // 10 minutes max

// ============================================================
// Timer helpers
// ============================================================
static auto t_start = std::chrono::steady_clock::now();
static void tick(const char* msg) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start).count();
    printf("[%7lldms] %s\n", (long long)ms, msg);
    fflush(stdout);
}

// ============================================================
// Resource Sampler (adapted from stress_driver.cpp)
// CPU% computed from delta CPU-time / delta wall-time
// Memory in MB (WorkingSet)
// ============================================================
struct ResourceUsage { double cpu_pct=0; long mem_mb=0; long peak_mem_mb=0; bool valid=false; };

#ifdef _WIN32
  #include <windows.h>
  #include <psapi.h>
  #pragma comment(lib, "psapi.lib")
#endif

static bool sample_raw(int pid, double& cpu_time_sec, long& mem_mb) {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                           FALSE, static_cast<DWORD>(pid));
    if (!h) return false;

    FILETIME ct, et, kt, ut;
    if (GetProcessTimes(h, &ct, &et, &kt, &ut)) {
        ULARGE_INTEGER k, u;
        k.LowPart = kt.dwLowDateTime; k.HighPart = kt.dwHighDateTime;
        u.LowPart = ut.dwLowDateTime; u.HighPart = ut.dwHighDateTime;
        cpu_time_sec = static_cast<double>(k.QuadPart + u.QuadPart) / 10'000'000.0;
    } else {
        cpu_time_sec = 0;
    }

    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) {
        mem_mb = static_cast<long>(pmc.WorkingSetSize / (1024 * 1024));
    } else {
        mem_mb = 0;
    }

    CloseHandle(h);
    return true;
#else
    char stat_path[64];
    snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
    FILE* f = std::fopen(stat_path, "r");
    if (!f) return false;

    long utime = 0, stime = 0, rss_pages = 0;
    if (std::fscanf(f, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u "
                       "%*u %*u %lu %lu %*d %*d %*d %*d %*d %*d "
                       "%*u %*u %ld",
                    &utime, &stime, &rss_pages) >= 3) {
        long ticks = sysconf(_SC_CLK_TCK);
        long page_sz = sysconf(_SC_PAGESIZE);
        cpu_time_sec = static_cast<double>(utime + stime) / ticks;
        mem_mb = rss_pages * page_sz / (1024 * 1024);
    }
    std::fclose(f);
    return true;
#endif
}

struct Sampler {
    std::atomic<bool> stop{false};
    std::thread w;
    std::vector<double> cs;   // CPU% samples
    std::vector<long>   ms;   // mem MB samples
    long peak_mem = 0;

    void start(int pid) {
        stop = false;
        cs.clear();
        ms.clear();
        peak_mem = 0;
        w = std::thread([this, pid] {
            double prev_cpu = -1.0;
            auto   prev_wall = std::chrono::steady_clock::now();

            while (!stop) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));

                auto now = std::chrono::steady_clock::now();
                double cur_cpu = 0;
                long   cur_mem = 0;
                if (sample_raw(pid, cur_cpu, cur_mem)) {
                    if (prev_cpu >= 0) {
                        double elapsed = std::chrono::duration<double>(now - prev_wall).count();
                        if (elapsed > 0.001) {
                            double pct = (cur_cpu - prev_cpu) / elapsed * 100.0;
                            if (pct >= 0) cs.push_back(pct);
                        }
                    }
                    if (cur_mem > peak_mem) peak_mem = cur_mem;
                    ms.push_back(cur_mem);
                    prev_cpu = cur_cpu;
                    prev_wall = now;
                }
            }
        });
    }

    ResourceUsage finish() {
        stop = true;
        if (w.joinable()) w.join();
        ResourceUsage a;
        if (!cs.empty()) {
            double s = 0;
            for (auto v : cs) s += v;
            a.cpu_pct = s / static_cast<double>(cs.size());
            a.valid = true;
        }
        if (!ms.empty()) {
            long s = 0;
            for (auto v : ms) s += v;
            a.mem_mb = s / static_cast<long>(ms.size());
            a.valid = true;
        }
        a.peak_mem_mb = peak_mem;
        return a;
    }
};

// ============================================================
// Simple hash for chunk verification
// ============================================================
static uint64_t hash_chunk(const char* data, size_t len) {
    uint64_t h = 14695981039346656037ULL;  // FNV offset
    for (size_t i = 0; i < len; ++i) {
        h ^= (unsigned char)data[i];
        h *= 1099511628211ULL;  // FNV prime
    }
    return h;
}

// ============================================================
// Copy statistics
// ============================================================
struct CopyStats {
    uint64_t total_bytes = 0;
    long long copy_ms = 0;
    long long verify_ms = 0;
    int chunks_copied = 0;
    int chunks_verified = 0;
    bool verify_ok = false;
    ResourceUsage res;
};

// ============================================================
// Phase 1: Sequential copy using async::read + async::write
//          with double-buffering for pipeline overlap
// ============================================================

task<> copy_coro(int src_fd, int dst_fd, coro_off_t file_size,
                 size_t chunk_size, CopyStats* stats, io_context* c) {
    // Allocate double buffer for pipeline: read buf[0] while writing buf[1]
    std::vector<char> buf0(chunk_size);
    std::vector<char> buf1(chunk_size);
    char* bufs[2] = { buf0.data(), buf1.data() };

    coro_off_t offset = 0;
    int cur = 0;  // current read buffer index
    bool has_pending_write = false;
    int pending_write_len = 0;
    int pending_write_buf = 0;
    uint64_t pending_write_offset = 0;

    auto t0 = std::chrono::steady_clock::now();
    tick("copy start");

    while (offset < file_size) {
        size_t to_read = (offset + (coro_off_t)chunk_size <= file_size)
                         ? chunk_size
                         : (size_t)(file_size - offset);

        // Read into current buffer at offset
        int nr = co_await async::read(src_fd, {bufs[cur], to_read}, (uint64_t)offset);
        CHK(nr > 0);
        CHK((size_t)nr == to_read);

        // If there's a pending write from previous iteration, do it now
        if (has_pending_write) {
            int nw = co_await async::write(dst_fd,
                {bufs[pending_write_buf], (size_t)pending_write_len},
                pending_write_offset);
            CHK(nw == pending_write_len);
            stats->chunks_copied++;
        }

        // Save current read as pending write for next iteration
        pending_write_buf = cur;
        pending_write_len = nr;
        pending_write_offset = (uint64_t)offset;
        has_pending_write = true;

        offset += nr;
        cur = 1 - cur;  // swap buffer
    }

    // Flush last pending write
    if (has_pending_write) {
        int nw = co_await async::write(dst_fd,
            {bufs[pending_write_buf], (size_t)pending_write_len},
            pending_write_offset);
        CHK(nw == pending_write_len);
        stats->chunks_copied++;
    }

    stats->copy_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    stats->total_bytes = (uint64_t)file_size;

    double gb = file_size / (1024.0 * 1024.0 * 1024.0);
    double mbps = stats->copy_ms > 0
        ? (file_size / (1024.0 * 1024.0)) / (stats->copy_ms / 1000.0) : 0;
    printf("  copy: %llu bytes (%.2f GB) in %lldms (%.0f MB/s), %d chunks\n",
           (unsigned long long)stats->total_bytes, gb,
           (long long)stats->copy_ms, mbps, stats->chunks_copied);

    tick("copy complete");
    c->can_stop();
}

// ============================================================
// Phase 2: Verify — re-read both files and compare hashes
// ============================================================

task<> verify_coro(int src_fd, int dst_fd, coro_off_t file_size,
                   size_t chunk_size, CopyStats* stats, io_context* c) {
    if (!g_verify) {
        stats->verify_ok = true;
        tick("verify skipped");
        c->can_stop();
        co_return;
    }

    std::vector<char> sbuf(chunk_size);
    std::vector<char> dbuf(chunk_size);

    auto t0 = std::chrono::steady_clock::now();
    tick("verify start");

    coro_off_t offset = 0;
    int mismatches = 0;

    while (offset < file_size) {
        size_t to_read = (offset + (coro_off_t)chunk_size <= file_size)
                         ? chunk_size
                         : (size_t)(file_size - offset);

        int nr_s = co_await async::read(src_fd, {sbuf.data(), to_read}, (uint64_t)offset);
        int nr_d = co_await async::read(dst_fd, {dbuf.data(), to_read}, (uint64_t)offset);

        CHK(nr_s > 0);
        CHK(nr_d > 0);
        CHK((size_t)nr_s == to_read);
        CHK((size_t)nr_d == to_read);

        uint64_t h_src = hash_chunk(sbuf.data(), to_read);
        uint64_t h_dst = hash_chunk(dbuf.data(), to_read);

        if (h_src != h_dst) {
            mismatches++;
            printf("  MISMATCH at offset %lld: src_hash=%016llx dst_hash=%016llx\n",
                   (long long)offset,
                   (unsigned long long)h_src,
                   (unsigned long long)h_dst);
        }

        stats->chunks_verified++;
        offset += nr_s;
    }

    stats->verify_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    stats->verify_ok = (mismatches == 0);

    printf("  verify: %d chunks checked, %d mismatches, %lldms\n",
           stats->chunks_verified, mismatches, (long long)stats->verify_ms);
    CHK(mismatches == 0);

    tick("verify complete");
    c->can_stop();
}

/// Timeout stopper
task<> timeout_stop(io_context* c, int sec) {
    co_await async::timeout(std::chrono::seconds(sec));
    printf("  [TIMEOUT %ds]\n", sec);
    c->can_stop();
}

// ============================================================
// CSV output
// ============================================================
static void save_csv(const CopyStats& stats, const std::string& src, const std::string& dst) {
    std::filesystem::create_directories("data");
    time_t now = std::time(nullptr);
    char fn[128];
    std::strftime(fn, sizeof(fn), "data/cp_stress_%Y%m%d_%H%M%S.csv", std::localtime(&now));

    std::ofstream f(fn);
    if (!f) return;
    f << "Source,Destination,Size_GB,Chunk_MB,Copy_ms,Copy_MBps,Verify_ms,Verify_OK,"
      << "Chunks_Copied,Chunks_Verified,CPU_pct,Avg_Mem_MB,Peak_Mem_MB\n";
    double gb = stats.total_bytes / (1024.0 * 1024.0 * 1024.0);
    double copy_mbps = stats.copy_ms > 0
        ? (stats.total_bytes / (1024.0 * 1024.0)) / (stats.copy_ms / 1000.0) : 0;
    f << src << ',' << dst << ','
      << gb << ','
      << g_chunk_size / (1024.0 * 1024.0) << ','
      << stats.copy_ms << ','
      << copy_mbps << ','
      << stats.verify_ms << ','
      << (stats.verify_ok ? "YES" : "NO") << ','
      << stats.chunks_copied << ','
      << stats.chunks_verified << ','
      << (stats.res.valid ? stats.res.cpu_pct : 0.0) << ','
      << (stats.res.valid ? stats.res.mem_mb : 0L) << ','
      << stats.res.peak_mem_mb << '\n';
    printf("  Data saved: %s\n", fn);
}

// ============================================================
// Argument parsing
// ============================================================
static void print_usage(const char* prog) {
    printf("Usage: %s [src] [dst] [options]\n", prog);
    printf("  src          Source file path (default: %s)\n", DEFAULT_SRC.c_str());
    printf("  dst          Destination file path (default: <Desktop>/<src_filename>)\n");
    printf("  --chunk MB   Chunk size in MB (default: 4)\n");
    printf("  --no-verify  Skip verification after copy\n");
    printf("  --timeout S  Timeout in seconds (default: 600)\n");
    printf("  --help       Show this help\n");
}

static void parse_args(int argc, char** argv) {
    g_src_path = DEFAULT_SRC;
    std::string desktop = get_default_desktop();
    // default dst: desktop + filename of src
    std::string fname = std::filesystem::path(DEFAULT_SRC).filename().string();
    g_dst_path = desktop + "/" + fname;

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "--chunk" && i + 1 < argc) {
            int mb = std::atoi(argv[++i]);
            if (mb > 0) g_chunk_size = (size_t)mb * 1024 * 1024;
        } else if (arg == "--no-verify") {
            g_verify = false;
        } else if (arg == "--timeout" && i + 1 < argc) {
            g_timeout_sec = std::atoi(argv[++i]);
        } else if (arg.substr(0, 2) == "--") {
            printf("Unknown option: %s\n", std::string(arg).c_str());
            print_usage(argv[0]);
            std::exit(1);
        } else {
            if (positional == 0) {
                g_src_path = argv[i];
                // update default dst if dst not yet set
                std::string f = std::filesystem::path(g_src_path).filename().string();
                g_dst_path = desktop + "/" + f;
            } else if (positional == 1) {
                g_dst_path = argv[i];
            }
            positional++;
        }
    }
}

// ============================================================
// Main
// ============================================================
int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    parse_args(argc, argv);

    printf("=== coronet cp_tool — async file copy stress test ===\n");
    printf("  src:    %s\n", g_src_path.c_str());
    printf("  dst:    %s\n", g_dst_path.c_str());
    printf("  chunk:  %zu MB\n", g_chunk_size / (1024 * 1024));
    printf("  verify: %s\n", g_verify ? "YES" : "NO");
    printf("  timeout: %ds\n", g_timeout_sec);
    printf("\n");

    // Check source file exists and get size
    if (!std::filesystem::exists(g_src_path)) {
        printf("FAIL: source file not found: %s\n", g_src_path.c_str());
        return 1;
    }

    // Open source file
    int src_fd = coro_open(g_src_path.c_str(), CORO_O_RDONLY);
    CHK(src_fd >= 0);
    coro_off_t file_size = coro_lseek(src_fd, 0, CORO_SEEK_END);
    CHK(file_size > 0);
    printf("  source size: %lld bytes (%.2f GB)\n\n",
           (long long)file_size, file_size / (1024.0 * 1024.0 * 1024.0));

    // Open/create destination file
    int dst_fd = coro_open(g_dst_path.c_str(),
                           CORO_O_RDWR | CORO_O_CREAT | CORO_O_TRUNC, 0644);
    if (dst_fd < 0) {
        printf("FAIL: cannot create destination file: %s\n", g_dst_path.c_str());
        printf("  (errno info: check path exists and is writable)\n");
        coro_close(src_fd);
        return 1;
    }

    // async::write with offset will naturally extend the file to full size.
    // No need to pre-truncate or seek — offset-based writes handle it.

    t_start = std::chrono::steady_clock::now();
    CopyStats stats{};

    // Start sampler for this process
    int pid =
#ifdef _WIN32
        (int)GetCurrentProcessId();
#else
        (int)getpid();
#endif

    Sampler sampler;
    sampler.start(pid);

    // ---- Phase 1: Copy ----
    printf("--- Phase 1: Async Copy ---\n");
    {
        io_context ctx;
        ctx.co_spawn(copy_coro(src_fd, dst_fd, file_size, g_chunk_size, &stats, &ctx));
        ctx.co_spawn(timeout_stop(&ctx, g_timeout_sec));
        ctx.start();
        ctx.join();
    }

    // ---- Phase 2: Verify ----
    if (g_verify) {
        printf("\n--- Phase 2: Verify ---\n");
        // Reset file offsets for verification read
        coro_lseek(src_fd, 0, CORO_SEEK_SET);
        coro_lseek(dst_fd, 0, CORO_SEEK_SET);

        {
            io_context ctx;
            ctx.co_spawn(verify_coro(src_fd, dst_fd, file_size, g_chunk_size, &stats, &ctx));
            ctx.co_spawn(timeout_stop(&ctx, g_timeout_sec));
            ctx.start();
            ctx.join();
        }
    } else {
        stats.verify_ok = true;
    }

    // Stop sampler and collect resource usage
    stats.res = sampler.finish();

    // Close files
    coro_close(src_fd);
    coro_close(dst_fd);

    // ---- Summary ----
    printf("\n=== Summary ===\n");
    double gb = stats.total_bytes / (1024.0 * 1024.0 * 1024.0);
    double copy_mbps = stats.copy_ms > 0
        ? (stats.total_bytes / (1024.0 * 1024.0)) / (stats.copy_ms / 1000.0) : 0;
    double verify_mbps = stats.verify_ms > 0
        ? (stats.total_bytes / (1024.0 * 1024.0)) / (stats.verify_ms / 1000.0) : 0;

    printf("  File:        %.2f GB (%llu bytes)\n", gb, (unsigned long long)stats.total_bytes);
    printf("  Chunk size:  %zu MB\n", g_chunk_size / (1024 * 1024));
    printf("  Copy:        %lldms (%.0f MB/s), %d chunks\n",
           (long long)stats.copy_ms, copy_mbps, stats.chunks_copied);
    if (g_verify) {
        printf("  Verify:      %lldms (%.0f MB/s), %d chunks, %s\n",
               (long long)stats.verify_ms, verify_mbps, stats.chunks_verified,
               stats.verify_ok ? "OK" : "FAILED");
    }
    if (stats.res.valid) {
        printf("  CPU:         %.1f%%\n", stats.res.cpu_pct);
        printf("  Memory:      avg %ld MB, peak %ld MB\n",
               stats.res.mem_mb, stats.res.peak_mem_mb);
    }

    // Save CSV
    save_csv(stats, g_src_path, g_dst_path);

    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start).count();
    printf("\n=== %s (total %lldms) ===\n",
           stats.verify_ok ? "ALL PASSED" : "FAILED",
           (long long)total_ms);

    return stats.verify_ok ? 0 : 1;
}
