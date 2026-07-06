# ============================================================
# Compiler options for coronet
# ============================================================

# Require C++20 as a target-level feature (works regardless of include order)
target_compile_features(coronet PUBLIC cxx_std_20)

# MSVC needs EHsc for C++ exception handling
if(MSVC)
    add_compile_options(/EHsc)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
    message(NOTICE "Setting default CMAKE_BUILD_TYPE to Release")
endif()

# Release build optimizations
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # 它会探测宿主机的 CPU 支持哪些扩展（如 AVX-512、AVX2、SSE4.2 等），并将这些指令集直接编译进二进制文件
    target_compile_options(coronet PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-march=native>)
    # Enable LTO when available
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_output)
    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

# MSVC: UTF-8 for Chinese comments in source files
target_compile_options(coronet PUBLIC
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)