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

# ---- Load generator + driver ----
add_executable(redis_loadgen  redis_loadgen.cpp)
target_link_libraries(redis_loadgen  PRIVATE coronet::coronet)
add_executable(stress_driver  stress_test_Driver.cpp)
target_link_libraries(stress_driver  PRIVATE coronet::coronet)
if(CORONET_HAS_ASIO)
    target_compile_definitions(stress_driver PRIVATE CORONET_HAS_ASIO)
endif()

# ============================================================
# CTest — 通过 --server name:binary:port 配置，添加新服务端只改这里
# ============================================================

# ST: 单线程对比
add_test(NAME stress_driver_ST COMMAND $<TARGET_FILE:stress_driver>
    --server "coronet_ST:redis_echo_ST:6380"
    --server "coronet_chain:redis_echo_chain:6381"
    $<$<BOOL:${CORONET_HAS_ASIO}>:--server>
    $<$<BOOL:${CORONET_HAS_ASIO}>:"ASIO_ST:redis_echo_asio_ST:6382">
    -n 10000 -c 50)
set_tests_properties(stress_driver_ST PROPERTIES TIMEOUT 60)

# MT: 多线程对比
add_test(NAME stress_driver_MT COMMAND $<TARGET_FILE:stress_driver>
    --server "coronet_MT(6):redis_echo_MT:6390"
    $<$<BOOL:${CORONET_HAS_ASIO}>:--server>
    $<$<BOOL:${CORONET_HAS_ASIO}>:"ASIO_MT(6):redis_echo_asio_MT:6391">
    -n 10000 -c 50)
set_tests_properties(stress_driver_MT PROPERTIES TIMEOUT 60)
