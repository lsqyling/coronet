# ============================================================
# External dependencies (bundled in extern/)
# ============================================================

# ---- liburingcxx (Linux, internal dependency) ----
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(liburingcxx QUIET)
    if(liburingcxx_FOUND)
        target_link_libraries(coronet PUBLIC liburingcxx::liburingcxx)
        message(STATUS "coronet: using external liburingcxx")
    else()
        if(EXISTS "${PROJECT_SOURCE_DIR}/extern/liburingcxx/CMakeLists.txt")
            add_subdirectory(extern/liburingcxx)
            target_link_libraries(coronet PUBLIC liburingcxx::liburingcxx)
            message(STATUS "coronet: using bundled liburingcxx")
        else()
            message(WARNING "coronet: liburingcxx not found; io_uring proactor may be incomplete")
        endif()
    endif()
endif()

# ---- mimalloc (optional) ----
if(CORONET_USE_MIMALLOC)
    find_package(mimalloc QUIET)
    if(mimalloc_FOUND)
        target_link_libraries(coronet PUBLIC mimalloc)
        target_compile_definitions(coronet PUBLIC CORONET_USE_MIMALLOC)
        message(STATUS "coronet: using mimalloc")
    else()
        message(WARNING "coronet: CORONET_USE_MIMALLOC=ON but mimalloc not found")
    endif()
endif()

# ---- googletest (for unit tests) ----
if(CORONET_BUILD_TESTS AND EXISTS "${PROJECT_SOURCE_DIR}/extern/googletest/CMakeLists.txt")
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(extern/googletest EXCLUDE_FROM_ALL)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    enable_testing()
endif()

# ---- Google Benchmark (for performance tests) ----
if(CORONET_BUILD_BENCHMARKS AND EXISTS "${PROJECT_SOURCE_DIR}/extern/benchmark/CMakeLists.txt")
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(extern/benchmark EXCLUDE_FROM_ALL)
endif()
