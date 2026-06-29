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
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define popen  _popen
#define pclose _pclose
#define PATH_SEP "\\"
#else
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#define PATH_SEP "/"
#endif

// ============================================================
// Config
// ============================================================
static int  g_requests    = 10000;
static int  g_concurrency = 50;
static bool g_verbose     = false;

struct Server {
    std::string name;     // 显示名称
    std::string binary;   // 可执行文件
    int  port;            // 监听端口
};

static std::vector<Server> g_servers;

// ============================================================
// Helpers
// ============================================================
static bool has_redis_benchmark() {
#ifdef _WIN32
    FILE* f = popen("where redis-benchmark 2>nul", "r");
#else
    FILE* f = popen("which redis-benchmark 2>/dev/null", "r");
#endif
    if (!f) return false;
    char buf[256]={}; bool ok=(fgets(buf,sizeof(buf),f)&&buf[0]!='\0'); pclose(f); return ok;
}

static std::string trim(const std::string& s) {
    size_t b=s.find_first_not_of(" \t\r\n"), e=s.find_last_not_of(" \t\r\n");
    return (b==std::string::npos)?"":s.substr(b,e-b+1);
}

static std::string run_cmd(const char* cmd) {
    std::string out; FILE* f=popen(cmd,"r"); if(!f)return out;
    char buf[1024]; while(fgets(buf,sizeof(buf),f)) out+=buf; pclose(f); return out;
}

static bool file_exists(const char* path) {
#ifdef _WIN32
    return GetFileAttributesA(path)!=INVALID_FILE_ATTRIBUTES;
#else
    return access(path,F_OK)==0;
#endif
}

// ============================================================
// Parse --server "name:binary:port"
// ============================================================
static bool parse_server(const char* arg, Server& s) {
    std::string a(arg);
    auto c1 = a.find(':');
    if (c1 == std::string::npos) return false;
    auto c2 = a.find(':', c1 + 1);
    if (c2 == std::string::npos) return false;
    s.name   = a.substr(0, c1);
    s.binary = a.substr(c1 + 1, c2 - c1 - 1);
    s.port   = std::atoi(a.substr(c2 + 1).c_str());
    return s.port > 0 && s.port < 65536;
}

// ============================================================
// Resource sampling
// ============================================================
struct ResourceUsage { double cpu_pct=0; long mem_kb=0; bool valid=false; };

static ResourceUsage sample_resources(int pid) {
    ResourceUsage ru; if(pid<=0)return ru;
#ifdef _WIN32
    char cmd[256]; std::snprintf(cmd,sizeof(cmd),
        "powershell -NoProfile -Command \"Get-Process -Id %d | "
        "Select @{N='C';E={[math]::Round($_.CPU,1)}},@{N='W';E={[math]::Round($_.WorkingSet64/1KB,0)}} | "
        "Format-Table -HideTableHeaders\" 2>nul",pid);
    std::string out=run_cmd(cmd); double cpu=0; long mem=0;
    if(!out.empty()&&std::sscanf(trim(out).c_str(),"%lf %ld",&cpu,&mem)>=2) {ru.cpu_pct=cpu;ru.mem_kb=mem;ru.valid=true;}
#else
    char cmd[128]; std::snprintf(cmd,sizeof(cmd),"top -b -n1 -p %d 2>/dev/null | tail -1",pid);
    std::string out=run_cmd(cmd); long p=0; double cpu=0; long mem=0;
    if(!out.empty()&&std::sscanf(out.c_str(),"%ld %*s %*d %*d %*d %ld %*d %*c %lf",&p,&mem,&cpu)>=3) {ru.cpu_pct=cpu;ru.mem_kb=mem;ru.valid=true;}
#endif
    return ru;
}

struct Sampler { std::atomic<bool> stop{false}; std::thread w; std::vector<double> cs; std::vector<long> ms;
    void start(int pid) { stop=false; w=std::thread([this,pid]{while(!stop){auto r=sample_resources(pid);if(r.valid){cs.push_back(r.cpu_pct);ms.push_back(r.mem_kb);}std::this_thread::sleep_for(std::chrono::milliseconds(500));}}); }
    ResourceUsage finish() { stop=true; if(w.joinable())w.join(); ResourceUsage a; if(!cs.empty()){double s=0;for(auto v:cs)s+=v;a.cpu_pct=s/cs.size();a.valid=true;} if(!ms.empty()){long s=0;for(auto v:ms)s+=v;a.mem_kb=s/(long)ms.size();a.valid=true;} return a; }
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

static bool spawn(Subprocess* sp, const char* binary, int port) {
    std::string path = "." PATH_SEP + std::string(binary);
#ifdef _WIN32
    path+=".exe";
    if(!file_exists(path.c_str())) { std::fprintf(stderr,"  [SKIP] %s not found\n",path.c_str()); return false; }
    STARTUPINFOA si{}; PROCESS_INFORMATION pi{}; si.cb=sizeof(si);
    std::string cmd=path+" "+std::to_string(port);
    char* buf=_strdup(cmd.c_str());
    if(!CreateProcessA(nullptr,buf,nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)) { std::fprintf(stderr,"  [FAIL] CreateProcess %s\n",binary); free(buf); return false; }
    free(buf); sp->h=pi.hProcess; sp->pid=GetProcessId(pi.hProcess); CloseHandle(pi.hThread);
#else
    if(!file_exists(path.c_str())) { std::fprintf(stderr,"  [SKIP] %s not found\n",path.c_str()); return false; }
    pid_t p=fork(); if(p<0){std::fprintf(stderr,"  [FAIL] fork %s\n",binary);return false;}
    if(p==0) { freopen("/dev/null","w",stdout); freopen("/dev/null","w",stderr); execl(path.c_str(),binary,std::to_string(port).c_str(),nullptr); _exit(127); }
    sp->pid=p;
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
            char cmd[128]; std::snprintf(cmd,sizeof(cmd),"redis-cli -p %d ping 2>/dev/null",port);
            if (run_cmd(cmd).find("PONG") != std::string::npos) return true;
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
// Load test
// ============================================================
struct LoadResult { bool ok; double rps; };

static LoadResult run_redis_benchmark(int port) {
    LoadResult r{}; char cmd[512];
    std::snprintf(cmd,sizeof(cmd),"redis-benchmark -p %d -n %d -c %d -t ping -q 2>&1",port,g_requests,g_concurrency);
    std::string out=run_cmd(cmd);
    if(const char* p=std::strstr(out.c_str(),"requests per second")) { const char* e=p;while(e>out.c_str()&&*(e-1)==' ')--e; const char* s=e;while(s>out.c_str()&&*(s-1)!=' ')--s; r.rps=std::strtod(s,nullptr); if(r.rps>0)r.ok=true; }
    if(g_verbose) std::printf("%s",out.c_str());
    return r;
}

static LoadResult run_loadgen(int port) {
    LoadResult r{}; char cmd[512];
    std::snprintf(cmd,sizeof(cmd),"./redis_loadgen -p %d -n %d -c %d 2>&1",port,g_requests,g_concurrency);
    std::string out=run_cmd(cmd);
    if(const char* p=std::strstr(out.c_str(),"RPS:")) { r.rps=std::strtod(p+4,nullptr); if(r.rps>0)r.ok=true; }
    if(!r.ok&&std::strstr(out.c_str(),"completed")){r.ok=true;r.rps=1.0;}
    if(g_verbose) std::printf("%s",out.c_str());
    return r;
}

// ============================================================
// Test runner — spawn, collect, kill
// ============================================================
struct TestResult {
    std::string name; int port;
    double rb_rps=0,lg_rps=0; bool rb_ok=false,lg_ok=false;
    ResourceUsage res;
};

static TestResult run_one(const Server& srv) {
    TestResult r; r.name=srv.name; r.port=srv.port;
    std::printf("  %-20s [port %-5d] ", srv.name.c_str(), srv.port); fflush(stdout);

    Subprocess sp{};
    if (!spawn(&sp, srv.binary.c_str(), srv.port)) { std::printf("SKIP\n"); return r; }
    if (!wait_ready(srv.port, 10)) { std::printf("FAIL (port not ready)\n"); kill_sp(&sp); return r; }

    bool has_rb = has_redis_benchmark();
    Sampler sampler;
    sampler.start(
#ifdef _WIN32
        sp.pid
#else
        sp.pid
#endif
    );

    if (has_rb) { auto lr=run_redis_benchmark(srv.port); r.rb_rps=lr.rps; r.rb_ok=lr.ok; }
    { auto lr=run_loadgen(srv.port); r.lg_rps=lr.rps; r.lg_ok=lr.ok; }

    r.res = sampler.finish();
    kill_sp(&sp);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (r.rb_ok || r.lg_ok) {
        std::printf("PASS");
        if(r.rb_ok) std::printf("  rb=%.0f",r.rb_rps);
        if(r.lg_ok) std::printf("  lg=%.0f",r.lg_rps);
        if(r.res.valid) std::printf("  cpu=%.1f%%  mem=%ldK",r.res.cpu_pct,r.res.mem_kb);
        std::printf("\n");
    } else { std::printf("FAIL\n"); }
    return r;
}

// ============================================================
// Summary + CSV
// ============================================================
static void summary(const std::vector<TestResult>& res) {
    std::printf("\n\33[1m══════ %zu servers | %d req x %d concurrent ══════\33[0m\n", res.size(), g_requests, g_concurrency);
    std::printf("  %-20s %8s %8s %8s %8s %s\n","Server","RB(RPS)","LG(RPS)","CPU%","Mem(KB)","Status");
    int p=0,f=0;
    for(auto& r:res){char rb[16]="-",lg[16]="-",cpu[16]="-",mem[16]="-";const char* s="FAIL";
        if(r.rb_ok)std::snprintf(rb,sizeof(rb),"%.0f",r.rb_rps); if(r.lg_ok)std::snprintf(lg,sizeof(lg),"%.0f",r.lg_rps);
        if(r.res.valid){std::snprintf(cpu,sizeof(cpu),"%.1f",r.res.cpu_pct);std::snprintf(mem,sizeof(mem),"%ld",r.res.mem_kb);}
        if(r.rb_ok||r.lg_ok){s="PASS";p++;}else{f++;}
        std::printf("  %-20s %8s %8s %8s %8s %s\n",r.name.c_str(),rb,lg,cpu,mem,s);}
    std::printf("  %-20s %8s %8s %8s %8s %d passed, %d failed\n","TOTAL","","","","",p,f);
}

static void csv(const std::vector<TestResult>& res) {
    std::system("mkdir -p data 2>/dev/null");
#ifdef _WIN32
    CreateDirectoryA("data",nullptr);
#endif
    time_t now=time(nullptr); char fn[128]; std::strftime(fn,sizeof(fn),"data/stress_%Y%m%d_%H%M%S.csv",localtime(&now));
    FILE* f=std::fopen(fn,"w"); if(!f)return;
    std::fprintf(f,"Server,Port,RB_RPS,LG_RPS,CPU%%,Mem_KB,Status\n");
    for(auto& r:res) std::fprintf(f,"%s,%d,%.0f,%.0f,%.1f,%ld,%s\n",r.name.c_str(),r.port,r.rb_ok?r.rb_rps:0.0,r.lg_ok?r.lg_rps:0.0,r.res.valid?r.res.cpu_pct:0.0,r.res.valid?r.res.mem_kb:0L,(r.rb_ok||r.lg_ok)?"PASS":"FAIL");
    std::fclose(f); std::printf("Data saved: %s\n",fn);
}

// ============================================================
// Main
// ============================================================
int main(int argc, char** argv) {
    for (int i=1;i<argc;++i) {
        if (std::strcmp(argv[i],"--server")==0 && i+1<argc) {
            Server s; if (parse_server(argv[++i], s)) g_servers.push_back(s);
        } else if (std::strcmp(argv[i],"-n")==0 && i+1<argc) g_requests=std::atoi(argv[++i]);
        else if (std::strcmp(argv[i],"-c")==0 && i+1<argc) g_concurrency=std::atoi(argv[++i]);
        else if (std::strcmp(argv[i],"-v")==0) g_verbose=true;
    }

    if (g_servers.empty()) { std::fprintf(stderr,"Usage: stress_driver --server name:binary:port [...] [-n N] [-c C] [-v]\n"); return 1; }

    std::printf("=== coronet Stress Driver ===\n");
    std::printf("Servers: %zu\n", g_servers.size());
    std::printf("Tool:    %s\n", has_redis_benchmark()?"redis-benchmark + redis_loadgen":"redis_loadgen only");
    std::printf("Load:    %d req x %d concurrent\n\n", g_requests, g_concurrency);

    std::vector<TestResult> results;
    int fails=0;
    for(auto& s : g_servers) { auto r=run_one(s); if(!r.rb_ok&&!r.lg_ok)fails++; results.push_back(r); }

    summary(results); csv(results);
    return fails>0?1:0;
}
