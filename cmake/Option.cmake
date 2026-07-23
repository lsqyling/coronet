# ============================================================
# Build options for coronet
# ============================================================

option(CORONET_USE_MIMALLOC "Use mimalloc as default allocator" OFF)
option(CORONET_IOURING "Use io_uring instead of epoll on Linux" OFF)
option(CORONET_BUILD_TESTS "Build unit tests (gtest)" OFF)
option(CORONET_BUILD_BENCHMARKS "Build benchmarks (Google Benchmark)" OFF)
option(CORONET_BUILD_STRESS_TESTS "Build stress/load tests (redis-benchmark or redis_loadgen)" OFF)
option(CORONET_BUILD_EXAMPLES "Build examples" OFF)
option(CORONET_DEVELOPER_MODE "Build all targets for developing" ON) # 开发分支模式，便于测试

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
    message(NOTICE "CORONET_DEVELOPER_MODE: ON, it means build all targets")
endif()
