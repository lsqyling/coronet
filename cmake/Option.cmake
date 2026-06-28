# ============================================================
# Build options for coronet
# ============================================================

option(CORONET_USE_MIMALLOC "Use mimalloc as default allocator" OFF)
option(CORONET_BUILD_TESTS "Build unit tests (gtest)" ${PROJECT_IS_TOP_LEVEL})
option(CORONET_BUILD_BENCHMARKS "Build benchmarks (Google Benchmark)" OFF)
option(CORONET_BUILD_EXAMPLES "Build examples" ${PROJECT_IS_TOP_LEVEL})
