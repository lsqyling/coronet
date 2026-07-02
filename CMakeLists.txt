cmake_minimum_required(VERSION 3.20)
project(coronet
        VERSION 1.0
        LANGUAGES CXX)

# ============================================================
# Library target
# ============================================================
add_library(coronet STATIC)
add_library(coronet::coronet ALIAS coronet)

target_include_directories(coronet
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

# ============================================================
# Modular configuration (order matters)
# ============================================================
include(cmake/Option.cmake)         # Build options
include(cmake/CompileOption.cmake)  # C++ standard, compiler flags, LTO
include(cmake/Platform.cmake)       # Platform detection, link libraries
include(cmake/Extra.cmake)          # External deps (liburingcxx, gtest, benchmark, mimalloc)
include(cmake/Develop.cmake)        # Tests, examples, benchmarks
include(cmake/Install.cmake)        # Install rules

# ============================================================
# Library sources
# ============================================================
add_subdirectory(src)

# ============================================================
# Package config (optional)
# ============================================================
if(EXISTS "${PROJECT_SOURCE_DIR}/cmake/templates/Config.cmake.in")
    include(CMakePackageConfigHelpers)
    configure_package_config_file(
        "${PROJECT_SOURCE_DIR}/cmake/templates/Config.cmake.in"
        "${PROJECT_BINARY_DIR}/coronetConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/coronet")
endif()
