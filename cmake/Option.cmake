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
