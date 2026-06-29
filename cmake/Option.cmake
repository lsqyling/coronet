# ============================================================
# Build options for coronet
# ============================================================

option(CORONET_USE_MIMALLOC "Use mimalloc as default allocator" OFF)
option(CORONET_IOURING "Use io_uring instead of epoll on Linux" OFF)
option(CORONET_BUILD_TESTS "Build unit tests (gtest)" ${PROJECT_IS_TOP_LEVEL})
option(CORONET_BUILD_BENCHMARKS "Build benchmarks (Google Benchmark)" ON)
option(CORONET_BUILD_STRESS_TESTS "Build stress/load tests (redis-benchmark or redis_loadgen)" OFF)
option(CORONET_BUILD_EXAMPLES "Build examples" ${PROJECT_IS_TOP_LEVEL})
