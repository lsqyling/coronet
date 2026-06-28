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
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-march=native)
    endif()
    # Enable LTO when available
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_output)
    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

# MSVC: UTF-8 for Chinese comments in source files
if(MSVC)
    target_compile_options(coronet PUBLIC /utf-8)
endif()
