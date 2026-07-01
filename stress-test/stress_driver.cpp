/// Unified stress test driver.
/// 通过 --server name:binary:port 参数配置测试目标，无需改代码即可添加新服务端。
///
/// Usage:
///   stress_driver --server name:binary:port [--server ...] [-n N] [-c C] [-v]
///
/// Example:
///   stress_driver --server "coronet_ST:redis_echo_ST:6380" --server "coronet_chain:redis_echo_chain:6381"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <io.h>
#pragma comment(lib, "kernel32.lib")
#else
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#endif

// ============================================================
// RAII pipe wrapper — replaces raw popen/pclose platform macros
// ============================================================
namespace {
struct PipeCmd {
    FILE* f = nullptr;
    explicit PipeCmd(FILE* pf) : f(pf) {}
    ~PipeCmd() { if (f) platform_pclose(f); }
    PipeCmd(const PipeCmd&) = delete;
    PipeCmd& operator=(const PipeCmd&) = delete;
    explicit operator bool() const noexcept { return f != nullptr; }
    FILE* handle() const noexcept { return f; }

private:
#ifdef _WIN32
    static void platform_pclose(FILE* pf) { _pclose(pf); }
#else
    static void platform_pclose(FILE* pf) { pclose(pf); }
#endif
};

inline PipeCmd popen_cmd(const char* cmd) {
#ifdef _WIN32
    return PipeCmd{_popen(cmd, "r")};
#else
    return PipeCmd{popen(cmd, "r")};
#endif
}
} // anonymous namespace

// ============================================================
// Config
// ============================================================
static int  g_requests    = 10000;
static int  g_concurrency = 50;
static bool g_verbose     = false;

struct Server {
    std::string name;     // 显示名称
    std::string binary;   // 可执行文件
    int  port;            // 监听端口（基端口）
    int  port_count = 1;  // 端口数量（多 worker 模式，默认 1）
};

static std::vector<Server> g_servers;

// ============================================================
// Helpers
// ============================================================
static bool has_redis_benchmark() {
#ifdef _WIN32
    auto pipe = popen_cmd("where redis-benchmark 2>nul");
#else
    auto pipe = popen_cmd("which redis-benchmark 2>/dev/null");
#endif
    if (!pipe) return false;
    char buf[256]{};
    return fgets(buf, sizeof(buf), pipe.handle()) && buf[0] != '\0';
}

static std::string run_cmd(const char* cmd) {
    auto pipe = popen_cmd(cmd);
    if (!pipe) return {};
    std::string out;
    char buf[1024];
    while (fgets(buf, sizeof(buf), pipe.handle())) out += buf;
    return out;
}

/// 获取当前可执行文件所在目录 / Get the directory containing the current executable
static std::filesystem::path get_exe_dir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    if (len > 0 && len < sizeof(buf)) {
        return std::filesystem::path(std::string(buf, len)).parent_path();
    }
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len != -1) {
        buf[len] = '\0';
        return std::filesystem::path(buf).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

/// Parse --server "name:binary:base_port[:port_count]"
static bool parse_server(std::string_view arg, Server& s) {
    auto c1 = arg.find(':');
    if (c1 == std::string_view::npos) return false;
    auto c2 = arg.find(':', c1 + 1);
    if (c2 == std::string_view::npos) return false;
    s.name   = arg.substr(0, c1);
    s.binary = arg.substr(c1 + 1, c2 - c1 - 1);
    // Parse base_port[:port_count]
    std::string_view tail = arg.substr(c2 + 1);
    auto c3 = tail.find(':');
    if (c3 != std::string_view::npos) {
        s.port       = std::atoi(std::string(tail.substr(0, c3)).c_str());
        s.port_count = std::atoi(std::string(tail.substr(c3 + 1)).c_str());
        if (s.port_count < 1) s.port_count = 1;
    } else {
        s.port = std::atoi(std::string(tail).c_str());
    }
    return s.port > 0 && s.port < 65536;
}

// ============================================================
// Resource sampling — Win32 API (no PowerShell dependency)
// CPU% computed from delta CPU-time / delta wall-time
// Memory in MB (WorkingSet)
// ============================================================
struct ResourceUsage { double cpu_pct=0; long mem_mb=0; bool valid=false; };

/// Sample raw process metrics. Returns true on success.
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
    auto stat_path = std::format("/proc/{}/stat", pid);
    FILE* f = std::fopen(stat_path.c_str(), "r");
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

    void start(int pid) {
        stop = false;
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
        return a;
    }
};

// ============================================================
// Subprocess
// ============================================================
struct Subprocess {
#ifdef _WIN32
    HANDLE h=nullptr; DWORD pid=0;
#else
    pid_t pid=0;
#endif
};

static bool spawn(Subprocess* sp, const char* binary, int port, int port_count) {
    auto exe_path = get_exe_dir() / binary;
#ifdef _WIN32
    exe_path += ".exe";
#endif
    if (!std::filesystem::exists(exe_path)) {
        std::fprintf(stderr, "  [SKIP] %s not found\n", exe_path.string().c_str());
        return false;
    }
    std::string path_str = exe_path.string();
    std::string args = std::to_string(port);
    if (port_count > 1) args += " " + std::to_string(port_count);
#ifdef _WIN32
    STARTUPINFOA si{}; PROCESS_INFORMATION pi{}; si.cb=sizeof(si);
    std::string cmd = path_str + " " + args;
    char* buf = _strdup(cmd.c_str());
    if (!CreateProcessA(nullptr, buf, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        std::fprintf(stderr, "  [FAIL] CreateProcess %s\n", binary);
        free(buf);
        return false;
    }
    free(buf);
    sp->h = pi.hProcess;
    sp->pid = GetProcessId(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    pid_t p = fork();
    if (p < 0) { std::fprintf(stderr, "  [FAIL] fork %s\n", binary); return false; }
    if (p == 0) {
        if (!freopen("/dev/null", "w", stdout)) _exit(127);
        if (!freopen("/dev/null", "w", stderr)) _exit(127);
        if (port_count > 1)
            execl(path_str.c_str(), binary, std::to_string(port).c_str(),
                  std::to_string(port_count).c_str(), nullptr);
        else
            execl(path_str.c_str(), binary, std::to_string(port).c_str(), nullptr);
        _exit(127);
    }
    sp->pid = p;
#endif
    return true;
}

static void kill_sp(Subprocess* sp) {
#ifdef _WIN32
    if(sp->h){TerminateProcess(sp->h,0);WaitForSingleObject(sp->h,3000);CloseHandle(sp->h);sp->h=nullptr;}
#else
    if(sp->pid>0){kill(sp->pid,SIGTERM);int s;waitpid(sp->pid,&s,0);sp->pid=0;}
#endif
}

/// 等待服务端端口就绪 / Wait for server port to be ready
static bool wait_ready(int port, int timeout_sec) {
    for (int i = 0; i < timeout_sec * 5; ++i) {
        if (has_redis_benchmark()) {
            auto cmd = std::format("redis-cli -p {} ping 2>/dev/null", port);
            if (run_cmd(cmd.c_str()).find("PONG") != std::string::npos) return true;
        } else {
            // Without redis-cli, just wait fixed time
            std::this_thread::sleep_for(std::chrono::seconds(timeout_sec));
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

// ============================================================
// Load test — single tool per run (redis-benchmark preferred)
// ============================================================
struct LoadResult { bool ok; double rps; };

/// Parse "XXX.XX requests per second" from redis-benchmark or redis_loadgen output
static LoadResult parse_bench_output(const std::string& out) {
    LoadResult r{};
    if (const char* p = std::strstr(out.c_str(), "requests per second")) {
        const char* e = p;
        while (e > out.c_str() && *(e - 1) == ' ') --e;
        const char* s = e;
        while (s > out.c_str() && *(s - 1) != ' ') --s;
        r.rps = std::strtod(s, nullptr);
        if (r.rps > 0) r.ok = true;
    }
    return r;
}

static LoadResult run_redis_benchmark(int port) {
    auto cmd = std::format("redis-benchmark -p {} -n {} -c {} -t ping -q 2>&1",
                           port, g_requests, g_concurrency);
    std::string out = run_cmd(cmd.c_str());
    if (g_verbose) std::printf("%s", out.c_str());
    return parse_bench_output(out);
}

static LoadResult run_loadgen(int port, int port_count) {
    auto lg_path = get_exe_dir() / "redis_loadgen";
#ifdef _WIN32
    lg_path += ".exe";
#endif
    std::string port_arg;
    if (port_count > 1)
        port_arg = std::format("{}-{}", port, port + port_count - 1);
    else
        port_arg = std::to_string(port);

    auto cmd = std::format("\"{}\" -p {} -n {} -c {} -P 1 -q 2>&1",
                           lg_path.string(), port_arg,
                           g_requests, g_concurrency);
    std::string out = run_cmd(cmd.c_str());
    if (g_verbose) std::printf("%s", out.c_str());
    return parse_bench_output(out);
}

// ============================================================
// Test runner — spawn, collect, kill
// ============================================================
struct TestResult {
    std::string name; int port;
    double rps = 0;
    bool   ok  = false;
    bool   used_rb = false;   // true = redis-benchmark, false = redis_loadgen
    ResourceUsage res;
};

static TestResult run_one(const Server& srv) {
    TestResult r; r.name=srv.name; r.port=srv.port;
    std::printf("  %-20s [port %-5d] ", srv.name.c_str(), srv.port); fflush(stdout);

    Subprocess sp{};
    if (!spawn(&sp, srv.binary.c_str(), srv.port, srv.port_count)) { std::printf("SKIP\n"); return r; }
    if (!wait_ready(srv.port, 10)) { std::printf("FAIL (port not ready)\n"); kill_sp(&sp); return r; }

    r.used_rb = has_redis_benchmark();
    Sampler sampler;
    sampler.start(sp.pid);

    LoadResult lr;
    if (r.used_rb)
        lr = run_redis_benchmark(srv.port);
    else
        lr = run_loadgen(srv.port, srv.port_count);

    r.rps = lr.rps;
    r.ok  = lr.ok;
    r.res = sampler.finish();
    kill_sp(&sp);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (r.ok) {
        std::printf("PASS  rps=%.0f", r.rps);
        if (r.used_rb) std::printf("  [redis-benchmark]");
        else           std::printf("  [redis_loadgen]");
        if (r.res.valid) std::printf("  cpu=%.1f%%  mem=%ldM", r.res.cpu_pct, r.res.mem_mb);
        std::printf("\n");
    } else { std::printf("FAIL\n"); }
    return r;
}

// ============================================================
// Summary + CSV
// ============================================================
static void summary(const std::vector<TestResult>& res) {
    std::printf("\n\33[1m══════ %zu servers | %d req x %d concurrent ══════\33[0m\n", res.size(), g_requests, g_concurrency);
    std::printf("  %-20s %8s %8s %8s %s\n","Server","RPS","CPU%","Mem(MB)","Status");
    int p=0,f=0;
    for(auto& r:res){char rps_str[16]="-",cpu[16]="-",mem[16]="-";const char* s="FAIL";
        if(r.ok){std::snprintf(rps_str,sizeof(rps_str),"%.0f",r.rps);s="PASS";p++;}else{f++;}
        if(r.res.valid){std::snprintf(cpu,sizeof(cpu),"%.1f",r.res.cpu_pct);std::snprintf(mem,sizeof(mem),"%ld",r.res.mem_mb);}
        std::printf("  %-20s %8s %8s %8s %s\n",r.name.c_str(),rps_str,cpu,mem,s);}
    std::printf("  %-20s %8s %8s %8s %d passed, %d failed\n","TOTAL","","","",p,f);
}

static void csv(const std::vector<TestResult>& res) {
    std::filesystem::create_directories("data");
    time_t now = std::time(nullptr);
    char fn[128];
    std::strftime(fn, sizeof(fn), "data/stress_%Y%m%d_%H%M%S.csv", std::localtime(&now));

    std::ofstream f(fn);
    if (!f) return;
    f << "Server,Port,RPS,CPU%,Mem_MB,Tool,Status\n";
    for (auto& r : res) {
        f << r.name << ',' << r.port << ','
          << (r.ok ? r.rps : 0.0) << ','
          << (r.res.valid ? r.res.cpu_pct : 0.0) << ','
          << (r.res.valid ? r.res.mem_mb : 0L) << ','
          << (r.used_rb ? "redis-benchmark" : "redis_loadgen") << ','
          << (r.ok ? "PASS" : "FAIL") << '\n';
    }
    std::printf("Data saved: %s\n", fn);
}

// ============================================================
// Main
// ============================================================
int main(int argc, char** argv) {
    std::span<char*> args(argv, static_cast<size_t>(argc));

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = args[i];
        if (arg == "--server" && i + 1 < argc) {
            Server s;
            if (parse_server(args[++i], s)) g_servers.push_back(s);
        } else if (arg == "-n" && i + 1 < argc) {
            g_requests = std::atoi(args[++i]);
        } else if (arg == "-c" && i + 1 < argc) {
            g_concurrency = std::atoi(args[++i]);
        } else if (arg == "-v") {
            g_verbose = true;
        }
    }

    if (g_servers.empty()) {
        std::fprintf(stderr, "Usage: stress_driver --server name:binary:port [...] "
                             "[-n N] [-c C] [-v]\n");
        return 1;
    }

    std::printf("=== coronet Stress Driver ===\n");
    std::printf("Servers: %zu\n", g_servers.size());
    bool has_rb = has_redis_benchmark();
    std::printf("Tool:    %s\n", has_rb ? "redis-benchmark" : "redis_loadgen");
    std::printf("Load:    %d req x %d concurrent\n\n", g_requests, g_concurrency);

    std::vector<TestResult> results;
    int fails = 0;
    for (auto& s : g_servers) {
        auto r = run_one(s);
        if (!r.ok) fails++;
        results.push_back(r);
    }

    summary(results); csv(results);
    return fails>0?1:0;
}
