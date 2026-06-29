# ============================================================
# Google Benchmark targets for coronet — CTest integration
# ============================================================

# ---- Task / Generator micro-benchmarks ----
set(coronet_micro_benchmarks
    task_benchmark
    generator_benchmark
)

foreach(bench_target ${coronet_micro_benchmarks})
    add_executable(${bench_target} ${bench_target}.cpp)
    target_link_libraries(${bench_target} PRIVATE coronet::coronet benchmark::benchmark benchmark::benchmark_main)
    add_test(NAME ${bench_target} COMMAND $<TARGET_FILE:${bench_target}>)
endforeach()
