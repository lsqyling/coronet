# ============================================================
# External dependencies (extern/ or FetchContent)
# ============================================================

# ---- Shared FetchContent download cache ----
# 固定使用 extern/_deps-<OS>/（gitignored），同一 OS 下所有 build 目录共享一份
# 下载缓存，避免每个新 build 目录都重新下载全部依赖。
# 按宿主系统分名（_deps-Linux / _deps-Windows）：本仓库常用于 WSL 与 Windows 原生
# 双系统共用同一 D 盘，若共享同一目录，缓存里的 CMakeCache.txt 记录的是另一侧的
# 路径（/mnt/d/... vs D:/...），CMake 会直接报目录不匹配错误。
# 必须用 FORCE 写入 cache：FetchContent.cmake 会在 include 时无条件
# set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH ...)，
# 不 FORCE 会在首次配置后把默认值污染成每目录缓存，重配即失效。
set(FETCHCONTENT_BASE_DIR "${PROJECT_SOURCE_DIR}/extern/_deps-${CMAKE_HOST_SYSTEM_NAME}"
    CACHE PATH "Directory under which to collect all populated content" FORCE)

# ---- liburingcxx (only when CORONET_IOURING=ON) ----
# 策略：优先使用系统 liburingcxx，找不到则 FetchContent 下载。
# 对齐 OpenSSL 的处理逻辑。
if(CORONET_IOURING)
    # 1. 优先使用系统已安装的 liburingcxx
    find_package(liburingcxx QUIET)
    if(liburingcxx_FOUND)
        target_link_libraries(coronet PUBLIC liburingcxx::liburingcxx)
        message(STATUS "coronet: using system liburingcxx (find_package)")
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
# 策略：优先系统 find_package（如 apt/vcpkg 的 libgtest），缺失则 FetchContent
# 下载（缓存在 extern/_deps-<OS>/，首次下载后复用）。与 liburingcxx/OpenSSL
# 对齐，三端（iocp/io_uring/epoll）一致。
if(CORONET_BUILD_TESTS)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        # 系统 gtest 只提供 GTest::gtest 命名空间目标，补别名供 test/ 目录链接
        add_library(gtest ALIAS GTest::gtest)
        add_library(gtest_main ALIAS GTest::gtest_main)
        message(STATUS "coronet: using system googletest (find_package)")
    else()
        # FetchContent 从 GitHub 下载（缓存在 extern/_deps-<OS>/，首次下载后复用）
        message(STATUS "coronet: googletest not found, trying FetchContent...")
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.15.2   # 与 submodule pin 一致
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(googletest)
        message(STATUS "coronet: using googletest via FetchContent (v1.15.2)")
    endif()
endif()

# ---- Google Benchmark (for performance tests) ----
# 策略：优先系统 find_package（如 apt/vcpkg 的 libbenchmark），缺失则 FetchContent
# 下载（缓存在 extern/_deps-<OS>/，首次下载后复用）。与 liburingcxx/OpenSSL 对齐。
if(CORONET_BUILD_BENCHMARKS)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    find_package(benchmark QUIET)
    if(benchmark_FOUND)
        message(STATUS "coronet: using system Google Benchmark (${benchmark_VERSION})")
    else()
        # ---- 跳过 benchmark 的配置期编译器探测 (CORONET_SKIP_DEPS_CHECKS) ----
        # benchmark v1.9.1 无条件运行 ~26 个 check_cxx_compiler_flag / try_run 探测
        # (AddCXXCompilerFlag.cmake + CXXFeatureCheck.cmake)。CMake 的 check_cxx_*
        # 宏在结果变量已定义时短路 (Internal/CheckSourceCompiles)，CXXFeatureCheck
        # 也有官方逃逸口 (if(DEFINED HAVE_${VAR}) return())，因此在已知工具链上
        # 预置与真实检查结果一致的变量值即可跳过全部探测。
        # 仅对 FetchContent 源码构建路径生效；系统包已预编译，无此问题。
        #
        # 语义注意：
        #   - CXXFeatureCheck 的 guard 把任何预置值强制为 1，故只能预置真实结果为 1 的变量；
        #   - GNU_POSIX_REGEX 在现代 glibc 上编译失败 (<gnuregex.h> 已移除)，绝不能
        #     预置为 1（re.h 会选中 GNU 分支导致编译错误），故意保留该检查（快速失败）。
        # 升级 benchmark 版本时需重新核对预置值与新版检查结果是否一致。
        if(CORONET_SKIP_DEPS_CHECKS)
            set(_coronet_known_toolchain OFF)
            if(WIN32 AND MSVC)
                set(_coronet_known_toolchain ON)
            elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
                if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "12") OR
                   (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "14"))
                    set(_coronet_known_toolchain ON)
                endif()
            endif()
            if(_coronet_known_toolchain)
                if(WIN32)
                    # MSVC：flag 探测不运行（不同分支）；只预置真实为 1 的 feature 结果，
                    # GNU/POSIX/PTHREAD_AFFINITY 保留（快速失败，语义与真实检查一致）
                    set(HAVE_STD_REGEX 1)
                    set(HAVE_STEADY_CLOCK 1)
                    set(HAVE_LIB_RT 0)      # MSVC 无 librt，检查结果为失败
                else()
                    # GCC/Clang (Linux/glibc)：预置值与本机检查结果逐一对齐
                    # (GCC 15 实测：15 success / 3 failed)
                    foreach(_flag WALL WEXTRA WSHADOW WFLOAT_EQUAL WOLD_STYLE_CAST
                                 WCONVERSION WERROR WSUGGEST_OVERRIDE PEDANTIC
                                 PEDANTIC_ERRORS FSTRICT_ALIASING WNO_DEPRECATED_DECLARATIONS
                                 WNO_DEPRECATED WSTRICT_ALIASING COVERAGE)
                        set(HAVE_CXX_FLAG_${_flag} TRUE)
                    endforeach()
                    set(HAVE_CXX_FLAG_WD654 FALSE)                # MSVC-only flag
                    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
                        set(HAVE_CXX_FLAG_WSHORTEN_64_TO_32 TRUE) # clang 支持
                        set(HAVE_CXX_FLAG_WTHREAD_SAFETY TRUE)    # clang 支持
                    else()
                        set(HAVE_CXX_FLAG_WSHORTEN_64_TO_32 FALSE) # clang-only flag
                        set(HAVE_CXX_FLAG_WTHREAD_SAFETY FALSE)    # GCC 不支持
                    endif()
                    # CXXFeatureCheck：只能预置真实值为 1 的变量
                    set(HAVE_STD_REGEX 1)
                    set(HAVE_POSIX_REGEX 1)
                    set(HAVE_STEADY_CLOCK 1)
                    set(HAVE_PTHREAD_AFFINITY 1)
                    # GNU_POSIX_REGEX 故意不预置（见上方语义注意）
                    set(HAVE_LIB_RT 1)
                endif()
            endif()
            unset(_coronet_known_toolchain)
        endif()

        # FetchContent 从 GitHub 下载（缓存在 extern/_deps-<OS>/，首次下载后复用）
        message(STATUS "coronet: benchmark not found, trying FetchContent...")
        include(FetchContent)
        FetchContent_Declare(
            benchmark
            GIT_REPOSITORY https://github.com/google/benchmark.git
            GIT_TAG        v1.9.1    # 与 submodule pin 一致
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(benchmark)
        message(STATUS "coronet: using Google Benchmark via FetchContent (v1.9.1)")
    endif()
endif()
