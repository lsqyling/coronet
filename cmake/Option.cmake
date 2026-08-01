# ============================================================
# Build options for coronet
# ============================================================

option(CORONET_WITH_TLS "Enable TLS support (requires OpenSSL)" ON)
option(CORONET_USE_MIMALLOC "Use mimalloc as default allocator" OFF)
option(CORONET_IOURING "Use io_uring instead of epoll on Linux" OFF)
option(CORONET_BUILD_TESTS "Build unit tests (gtest)" OFF)
option(CORONET_BUILD_BENCHMARKS "Build benchmarks (Google Benchmark)" OFF)
option(CORONET_BUILD_STRESS_TESTS "Build stress/load tests (redis-benchmark or redis_loadgen)" OFF)
option(CORONET_BUILD_EXAMPLES "Build examples" OFF)
# 对成熟的第三方库（benchmark 等）跳过配置期编译器特性探测（try_compile/try_run）。
# 仅对已知工具链生效（GCC/Clang on Linux, MSVC on Windows），其余工具链照常探测。
option(CORONET_SKIP_DEPS_CHECKS "Skip configure-time feature probes of mature third-party deps" ON)
# ---- 自动检测 git 分支, main/master 默认关闭开发模式 ----
execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE _coronet_git_branch
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if("${_coronet_git_branch}" STREQUAL "main" OR "${_coronet_git_branch}" STREQUAL "master")
    set(CORONET_DEVELOPER_MODE_DEFAULT OFF)
else()
    set(CORONET_DEVELOPER_MODE_DEFAULT ON)
endif()

set(CORONET_DEVELOPER_MODE ${CORONET_DEVELOPER_MODE_DEFAULT}
    CACHE BOOL "Build all targets for developing (auto-detected from git branch)")

# ============================================================
# Developer mode: enable all sub-targets
# NOTE: Must run BEFORE Extra.cmake so that external deps
#       (googletest, benchmark) are added when needed.
# ============================================================
if(CORONET_DEVELOPER_MODE)
    set(CORONET_BUILD_EXAMPLES ON)
    set(CORONET_BUILD_TESTS ON)
    set(CORONET_BUILD_BENCHMARKS ON)
    set(CORONET_BUILD_STRESS_TESTS ON)
    message(NOTICE "CORONET_DEVELOPER_MODE: ON (branch '${_coronet_git_branch}'), it means build all targets")
else()
    message(NOTICE "CORONET_DEVELOPER_MODE: OFF (branch '${_coronet_git_branch}'), building coronet library only")
endif()
