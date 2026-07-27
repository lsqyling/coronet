# ============================================================
# External dependencies (bundled in extern/)
# ============================================================

# ---- liburingcxx (only when CORONET_IOURING=ON) ----
if(CORONET_IOURING)
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
            message(FATAL_ERROR "coronet: CORONET_IOURING=ON but liburingcxx not found")
        endif()
    endif()
endif()

# ---- OpenSSL (for TLS support) ----
# 策略：优先使用系统 OpenSSL，找不到则 FetchContent 下载编译。
# Linux 上 find_package 几乎总是成功；fallback 主要服务 Windows 无 OpenSSL 环境。
if(CORONET_WITH_TLS)
    # 1. 优先使用系统已安装的 OpenSSL
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        target_link_libraries(coronet PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(coronet PUBLIC CORONET_HAS_TLS)
        message(STATUS "coronet: TLS enabled (system OpenSSL ${OpenSSL_VERSION})")
    else()
        # 2. Fallback: FetchContent 下载 OpenSSL 源码并编译
        #    需要 Perl（OpenSSL 构建脚本依赖）
        message(STATUS "coronet: System OpenSSL not found, trying FetchContent...")

        find_program(PERL_EXECUTABLE perl)
        if(NOT PERL_EXECUTABLE)
            message(FATAL_ERROR
                "coronet: Building OpenSSL from source requires Perl.\n"
                "  Options:\n"
                "  1. Install Perl (e.g. Strawberry Perl on Windows)\n"
                "  2. Install OpenSSL via system package manager:\n"
                "     - Windows: vcpkg install openssl (with toolchain file)\n"
                "     - Windows: ShiningLight installer (https://slproweb.com/products/Win32OpenSSL.html)\n"
                "     - Linux:   apt install libssl-dev / yum install openssl-devel\n"
                "     - macOS:   brew install openssl\n"
                "  3. Disable TLS: cmake -DCORONET_WITH_TLS=OFF ..")
        endif()

        include(FetchContent)
        FetchContent_Declare(
            openssl
            GIT_REPOSITORY https://github.com/openssl/openssl.git
            GIT_TAG        openssl-3.4.0
            GIT_SHALLOW    TRUE
        )

        # OpenSSL CMake 构建选项：仅构建库，不构建应用和测试
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(OPENSSL_BUILD_APPS OFF CACHE BOOL "" FORCE)
        set(OPENSSL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(OPENSSL_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)

        FetchContent_MakeAvailable(openssl)

        target_link_libraries(coronet PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(coronet PUBLIC CORONET_HAS_TLS)
        message(STATUS "coronet: TLS enabled (bundled OpenSSL via FetchContent)")
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
endif()

# ---- Google Benchmark (for performance tests) ----
if(CORONET_BUILD_BENCHMARKS AND EXISTS "${PROJECT_SOURCE_DIR}/extern/benchmark/CMakeLists.txt")
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(extern/benchmark)
endif()
