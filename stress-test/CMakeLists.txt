# ============================================================
# Stress / load test targets — CTest integration
# ============================================================
# 添加新服务端只需: add_executable + add_test 中追加 --server 参数
# stress_driver 代码无需修改。

# ---- ASIO (conditional) ----
if(EXISTS "${PROJECT_SOURCE_DIR}/extern/asio/asio/include/asio.hpp")
    add_library(asio INTERFACE)
    target_include_directories(asio INTERFACE "${PROJECT_SOURCE_DIR}/extern/asio/asio/include")
    target_compile_definitions(asio INTERFACE ASIO_STANDALONE)
    set(CORONET_HAS_ASIO TRUE)
else()
    set(CORONET_HAS_ASIO FALSE)
endif()

# ---- Server binaries ----
add_executable(redis_echo_ST     redis_echo_ST.cpp)
target_link_libraries(redis_echo_ST     PRIVATE coronet::coronet)
add_executable(redis_echo_chain  redis_echo_chain.cpp)
target_link_libraries(redis_echo_chain  PRIVATE coronet::coronet)
add_executable(redis_echo_MT     redis_echo_MT.cpp)
target_link_libraries(redis_echo_MT     PRIVATE coronet::coronet)

if(CORONET_HAS_ASIO)
    add_executable(redis_echo_asio_ST redis_echo_asio_ST.cpp)
    target_link_libraries(redis_echo_asio_ST PRIVATE asio)
    if(WIN32)
        target_compile_definitions(redis_echo_asio_ST PRIVATE _WIN32_WINNT=0x0A00)
        target_link_libraries(redis_echo_asio_ST PRIVATE ws2_32 mswsock)
    endif()
    add_executable(redis_echo_asio_MT redis_echo_asio_MT.cpp)
    target_link_libraries(redis_echo_asio_MT PRIVATE asio)
    if(WIN32)
        target_compile_definitions(redis_echo_asio_MT PRIVATE _WIN32_WINNT=0x0A00)
        target_link_libraries(redis_echo_asio_MT PRIVATE ws2_32 mswsock)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(redis_echo_asio_MT PRIVATE Threads::Threads)
    endif()
endif()

# ---- Stress driver ----
add_executable(stress_driver stress_driver.cpp)
target_link_libraries(stress_driver  PRIVATE coronet::coronet)
if(CORONET_HAS_ASIO)
    target_compile_definitions(stress_driver PRIVATE CORONET_HAS_ASIO)
endif()

# ---- Benchmark scripts are copied to build dir by cmake/Develop.cmake ----

# ============================================================
# Standalone stress tests (moved from test/)
# ============================================================

# Cross-platform stress tests (channel + combinator + file I/O)
set(coronet_stress_tests
    channel_stress
    combinator_stress
    lazy_io_comprehensive
    async_io_iso_stress
    cp_tool
)
foreach(test_target ${coronet_stress_tests})
    add_executable(${test_target} ${test_target}.cpp)
    target_link_libraries(${test_target} PRIVATE coronet::coronet)
    coronet_add_test(${test_target} COMMAND $<TARGET_FILE:${test_target}>)
    set_tests_properties(${test_target} PROPERTIES TIMEOUT 120)
endforeach()

# cp_tool: needs shell32 (SHGetFolderPath) and psapi (GetProcessMemoryInfo) on Windows
if(WIN32)
    target_link_libraries(cp_tool PRIVATE shell32 psapi)
endif()
# cp_tool tests copy a 5GB ISO — needs extended timeout
set_tests_properties(cp_tool PROPERTIES TIMEOUT 1200)

# ============================================================
# CTest — 通过 --server name:binary:port 配置，添加新服务端只改这里
# ============================================================

# ST: 单线程对比（使用高端口 17000+ 避免与常见服务冲突）
# Single-threaded comparison (high ports to avoid conflicts)
coronet_add_test(stress_driver_ST COMMAND $<TARGET_FILE:stress_driver>
    --server "coronet_ST:redis_echo_ST:17080"
    --server "coronet_chain:redis_echo_chain:17081"
    $<$<BOOL:${CORONET_HAS_ASIO}>:--server>
    $<$<BOOL:${CORONET_HAS_ASIO}>:"ASIO_ST:redis_echo_asio_ST:17082">
    -n 10000 -c 50)
set_tests_properties(stress_driver_ST PROPERTIES TIMEOUT 60)

# MT: 多线程对比（使用高端口 17090+ 避免与常见服务冲突）
# Multi-threaded comparison (high ports to avoid conflicts)
coronet_add_test(stress_driver_MT COMMAND $<TARGET_FILE:stress_driver>
    --server "coronet_MT(6):redis_echo_MT:17090"
    $<$<BOOL:${CORONET_HAS_ASIO}>:--server>
    $<$<BOOL:${CORONET_HAS_ASIO}>:"ASIO_MT\(6\):redis_echo_asio_MT:17091">
    -n 10000 -c 50)
set_tests_properties(stress_driver_MT PROPERTIES TIMEOUT 120)
