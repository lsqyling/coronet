# ============================================================
# Test auto-discovery for coronet
# ============================================================

# GoogleTest unit tests (each file uses gtest framework)
set(coronet_gtest_tests
    task_gtest
    generator_gtest
    proactor_gtest
    channel_gtest
    shared_task_gtest
)

foreach(test_target ${coronet_gtest_tests})
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${test_target}.cpp")
        add_executable(${test_target} ${test_target}.cpp)
        target_link_libraries(${test_target} PRIVATE coronet::coronet gtest gtest_main)
        add_test(NAME ${test_target} COMMAND ${test_target})
    endif()
endforeach()

# Standalone integration tests (each has its own main and assert-based verification)
set(coronet_int_tests
    ft_task
    move_shared_task
    generator_test
    coro_lifetime
    echo_server
    pingpong_client
)

# stress_test uses POSIX sockets (sys/socket.h) — Linux only
if(NOT WIN32)
    list(APPEND coronet_int_tests stress_test)
endif()

foreach(test_target ${coronet_int_tests})
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${test_target}.cpp")
        add_executable(${test_target} ${test_target}.cpp)
        target_link_libraries(${test_target} PRIVATE coronet::coronet)
        add_test(NAME ${test_target} COMMAND ${test_target})
    endif()
endforeach()

# Google Benchmark targets
set(coronet_benchmarks
    task_benchmark
    generator_benchmark
)

if(CORONET_BUILD_BENCHMARKS)
    foreach(bench_target ${coronet_benchmarks})
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${bench_target}.cpp")
            add_executable(${bench_target} ${bench_target}.cpp)
            target_link_libraries(${bench_target} PRIVATE coronet::coronet benchmark::benchmark benchmark::benchmark_main)
        endif()
    endforeach()
endif()
