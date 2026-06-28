# ============================================================
# Benchmark targets for coronet vs ASIO comparison
# ============================================================

# coronet coroutine-based Redis echo server (single-threaded, separate send+recv)
add_executable(redis_echo_coro redis_echo_coro.cpp)
target_link_libraries(redis_echo_coro PRIVATE coronet::coronet)

# coronet coroutine-based Redis echo server (single-threaded, chained co_await)
add_executable(redis_echo_chain redis_echo_chain.cpp)
target_link_libraries(redis_echo_chain PRIVATE coronet::coronet)

# coronet multi-threaded Redis echo server (cross-thread co_spawn)
add_executable(redis_echo_MT redis_echo_MT.cpp)
target_link_libraries(redis_echo_MT PRIVATE coronet::coronet)

# coronet multi-threaded Redis echo server (multi-port, no cross-thread spawn)
add_executable(redis_echo_MT_mp redis_echo_MT_reuseport.cpp)
target_link_libraries(redis_echo_MT_mp PRIVATE coronet::coronet)

# ASIO multi-threaded Redis echo server
if(EXISTS "${PROJECT_SOURCE_DIR}/extern/asio/asio/include/asio.hpp")
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

# ASIO callback-based Redis echo server
if(EXISTS "${PROJECT_SOURCE_DIR}/extern/asio/asio/include/asio.hpp")
    add_library(asio INTERFACE)
    target_include_directories(asio INTERFACE
        "${PROJECT_SOURCE_DIR}/extern/asio/asio/include")
    # ASIO can work standalone (without Boost) if ASIO_STANDALONE is defined
    target_compile_definitions(asio INTERFACE ASIO_STANDALONE)

    add_executable(redis_echo_asio redis_echo_asio.cpp)
    target_link_libraries(redis_echo_asio PRIVATE asio)
    if(WIN32)
        target_compile_definitions(redis_echo_asio PRIVATE _WIN32_WINNT=0x0A00)
        target_link_libraries(redis_echo_asio PRIVATE ws2_32 mswsock)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(redis_echo_asio PRIVATE Threads::Threads)
    endif()
endif()

# Custom TCP load generator (for Windows or when redis-benchmark not available)
add_executable(redis_loadgen redis_loadgen.cpp)
target_link_libraries(redis_loadgen PRIVATE coronet::coronet)
