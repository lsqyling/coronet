# ============================================================
# Stress / load test targets — CTest integration
# ============================================================
# 添加新服务端只需: add_executable + add_test 中追加 --server 参数
# stress_driver 代码无需修改。

# ---- ASIO (conditional) ----
# 策略：优先 find_package 独立 asio（如 vcpkg/Conan 提供 asio::asio），缺失则
# FetchContent 下载（缓存在 extern/_deps-<OS>/，首次下载后复用）。
# asio 是 header-only 项目，仓库没有根 CMakeLists.txt（autotools 构建），
# 因此 FetchContent 只下载源码（MakeAvailable 对无 CMakeLists.txt 的依赖
# 不会 add_subdirectory，CMake >= 3.18）。统一包装为 INTERFACE target `asio`
# 供下方服务器链接；find_package 分支链接 asio::asio。
# 历史上用 GitHub archive URL（支持任意 SHA），因为 GIT_SHALLOW + GIT_TAG=SHA
# 在 CMake 4.x 有 bug（git clone 报 unable to read tree，实测复现）；但 URL 走
# CMake 内部下载器，在 Windows 上会卡死（下载文件 0 字节、无进度）。故改用
# git clone；SHA pin 不能 shallow，用全量 clone（首次下载后走共享缓存）。
set(_asio_source_dir "")
find_package(asio QUIET)
if(asio_FOUND AND TARGET asio::asio)
    message(STATUS "coronet: using system asio (find_package)")
else()
    message(STATUS "coronet: asio not found, trying FetchContent...")
    include(FetchContent)
    FetchContent_Declare(
        asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG        bd500f0a018db9a845ebaaed5c0318343ae9f497
        GIT_SHALLOW    FALSE
    )
    FetchContent_MakeAvailable(asio)
    set(_asio_source_dir "${asio_SOURCE_DIR}")
    message(STATUS "coronet: using asio via FetchContent (bd500f0a)")
endif()

if(TARGET asio::asio)
    # 系统独立 asio：链接 asio::asio
    add_library(asio INTERFACE)
    target_link_libraries(asio INTERFACE asio::asio)
    set(CORONET_HAS_ASIO TRUE)
elseif(EXISTS "${_asio_source_dir}/asio/include/asio.hpp")
    add_library(asio INTERFACE)
    target_include_directories(asio INTERFACE "${_asio_source_dir}/asio/include")
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
    # 协程版（C1000K 同编程模型对比）— mirrors redis_echo_ST/MT.cpp structure:
    # per-connection coroutine frame with 4096B buffer, apples-to-apples with coronet.
    # asio INTERFACE target 不传播 C++20 特性，需显式开启（协程依赖 __cpp_coroutines）。
    # 回调版源文件（redis_echo_asio_ST/MT.cpp）保留在树内但不编译（对比基线，
    # 不再需要时可直接删除）。
    add_executable(redis_echo_asio_coro_ST redis_echo_asio_coro_ST.cpp)
    target_compile_features(redis_echo_asio_coro_ST PRIVATE cxx_std_20)
    target_link_libraries(redis_echo_asio_coro_ST PRIVATE asio)
    if(WIN32)
        target_compile_definitions(redis_echo_asio_coro_ST PRIVATE _WIN32_WINNT=0x0A00)
        target_link_libraries(redis_echo_asio_coro_ST PRIVATE ws2_32 mswsock)
    endif()
    add_executable(redis_echo_asio_coro_MT redis_echo_asio_coro_MT.cpp)
    target_compile_features(redis_echo_asio_coro_MT PRIVATE cxx_std_20)
    target_link_libraries(redis_echo_asio_coro_MT PRIVATE asio)
    if(WIN32)
        target_compile_definitions(redis_echo_asio_coro_MT PRIVATE _WIN32_WINNT=0x0A00)
        target_link_libraries(redis_echo_asio_coro_MT PRIVATE ws2_32 mswsock)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(redis_echo_asio_coro_MT PRIVATE Threads::Threads)
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
)
foreach(test_target ${coronet_stress_tests})
    add_executable(${test_target} ${test_target}.cpp)
    target_link_libraries(${test_target} PRIVATE coronet::coronet)
    coronet_add_test(${test_target} COMMAND $<TARGET_FILE:${test_target}>)
    set_tests_properties(${test_target} PROPERTIES TIMEOUT 120)
endforeach()

# ============================================================
# cp_tool — async file copy stress test (parameterized)
# ============================================================
# 通过 cmake -D 传入参数，无需修改代码中的硬编码路径:
#   cmake -S . -B build \
#       -DCP_TOOL_SRC=/path/to/source.iso \
#       -DCP_TOOL_DST=/path/to/dest.iso \
#       -DCP_TOOL_CHUNK_MB=8
#   ctest --test-dir build -R cp_tool -V
#
# 不传参数时使用 cp_tool 内置默认值（源码 DEFAULT_SRC / get_default_desktop）
# ISO 测试文件路径按平台区分（自动化硬编码默认值，可 cmake -D 覆盖）:
#   Windows: D:/dev/Downloads/...    Linux (WSL): /mnt/d/dev/Downloads/...
if(WIN32)
    set(CP_TOOL_DEFAULT_SRC "D:/dev/Downloads/windows_10_professional_x64_2026.iso")
    set(CP_TOOL_DEFAULT_DST "C:/Users/10580/AppData/windows_10_professional_x64_2026.iso")
else()
    set(CP_TOOL_DEFAULT_SRC "/mnt/d/dev/Downloads/windows_10_professional_x64_2026.iso")
    set(CP_TOOL_DEFAULT_DST "/mnt/c/Users/10580/AppData/windows_10_professional_x64_2026.iso")
endif()
set(CP_TOOL_SRC "${CP_TOOL_DEFAULT_SRC}" CACHE STRING "cp_tool: source file path")
set(CP_TOOL_DST "${CP_TOOL_DEFAULT_DST}" CACHE STRING "cp_tool: destination file path")
set(CP_TOOL_CHUNK_MB "4" CACHE STRING "cp_tool: chunk size in MB (default: 4)")
set(CP_TOOL_NO_VERIFY OFF CACHE BOOL "cp_tool: skip verification after copy")
set(CP_TOOL_TIMEOUT_SEC "600" CACHE STRING "cp_tool: timeout in seconds (default: 600)")

add_executable(cp_tool cp_tool.cpp)
target_link_libraries(cp_tool PRIVATE coronet::coronet)

# cp_tool: needs shell32 (SHGetFolderPath) and psapi (GetProcessMemoryInfo) on Windows
if(WIN32)
    target_link_libraries(cp_tool PRIVATE shell32 psapi)
endif()

# Build cp_tool command with optional arguments
set(cp_tool_args "")

# Positional args: src dst
if(CP_TOOL_SRC)
    list(APPEND cp_tool_args "${CP_TOOL_SRC}")
endif()
if(CP_TOOL_DST)
    list(APPEND cp_tool_args "${CP_TOOL_DST}")
endif()

# Named options
list(APPEND cp_tool_args "--chunk" "${CP_TOOL_CHUNK_MB}")
if(CP_TOOL_NO_VERIFY)
    list(APPEND cp_tool_args "--no-verify")
endif()
list(APPEND cp_tool_args "--timeout" "${CP_TOOL_TIMEOUT_SEC}")

coronet_add_test(cp_tool COMMAND $<TARGET_FILE:cp_tool> ${cp_tool_args})

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
    $<$<BOOL:${CORONET_HAS_ASIO}>:ASIO_coro_ST:redis_echo_asio_coro_ST:17082>
    -n 10000 -c 50)
set_tests_properties(stress_driver_ST PROPERTIES TIMEOUT 60)

# MT: 多线程对比（使用高端口 17090+ 避免与常见服务冲突）
# Multi-threaded comparison (high ports to avoid conflicts)
coronet_add_test(stress_driver_MT COMMAND $<TARGET_FILE:stress_driver>
    --server "coronet_MT(6):redis_echo_MT:17090"
    $<$<BOOL:${CORONET_HAS_ASIO}>:--server>
    $<$<BOOL:${CORONET_HAS_ASIO}>:ASIO_coro_MT\(6\):redis_echo_asio_coro_MT:17091>
    -n 10000 -c 50)
set_tests_properties(stress_driver_MT PROPERTIES TIMEOUT 120)
