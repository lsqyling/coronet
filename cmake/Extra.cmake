# ============================================================
# External dependencies (bundled in extern/)
# ============================================================

# ---- liburingcxx (only when CORONET_IOURING=ON) ----
# 策略：优先使用系统 liburingcxx，找不到则 FetchContent 下载。
# 对齐 OpenSSL 的处理逻辑。
if(CORONET_IOURING)
    # 1. 优先使用系统已安装的 liburingcxx
    find_package(liburingcxx QUIET)
    if(liburingcxx_FOUND)
        target_link_libraries(coronet PUBLIC liburingcxx::liburingcxx)
        message(STATUS "coronet: using external liburingcxx")
    else()
        # 2. Fallback: FetchContent 从 GitHub 下载
        message(STATUS "coronet: liburingcxx not found, trying FetchContent...")
        include(FetchContent)
        FetchContent_Declare(
            liburingcxx
            GIT_REPOSITORY https://github.com/Codesire-Deng/liburingcxx.git
            GIT_TAG        main
            GIT_SHALLOW    TRUE
            # coronet: disable IORING_ENTER_REGISTERED_RING (incompatible with WSL2)
            # This prevents io_uring_enter() from returning EINVAL on WSL2 kernels.
            # The liburingcxx config hardcodes this flag for kernel >= 5.18, but WSL2's
            # io_uring implementation does not support it.
            # Also removes deprecated <cstdbool> include (triggers -Wcpp on GCC 15+).
            # 同时移除已废弃的 <cstdbool> 头文件（GCC 15+ 触发 -Wcpp 警告）。
            PATCH_COMMAND sed -i
                -e "s/using_register_ring_fd = is_kernel_reach(5, 18)/using_register_ring_fd = false/"
                include/uring/uring.hpp
                &&
                sed -i "/#include <cstdbool>/d" include/uring/syscall.hpp
        )
        FetchContent_MakeAvailable(liburingcxx)
        target_link_libraries(coronet PUBLIC liburingcxx::liburingcxx)
        # 将 liburingcxx 加入 coronet_targets 导出集
        # FetchContent 创建的是本地 target，不在导出集中会导致：
        #   install(EXPORT "coronet_targets" ...) includes target "coronet"
        #   which requires target "liburingcxx" that is not in any export set.
        install(TARGETS liburingcxx EXPORT coronet_targets)
        message(STATUS "coronet: using bundled liburingcxx (FetchContent)")
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
