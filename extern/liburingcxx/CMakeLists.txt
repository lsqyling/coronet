cmake_minimum_required(VERSION 3.10.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(liburingcxx VERSION 0.9.0 LANGUAGES CXX)

if (NOT "${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
    message(WARNING "io_uring is only supported by Linux, but the target OS is ${CMAKE_SYSTEM_NAME}.")
endif()

add_library(liburingcxx INTERFACE)
add_library(liburingcxx::liburingcxx ALIAS liburingcxx)

target_include_directories(
    liburingcxx
    INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)

# coronet: streamlined - no examples/tests for bundled copy

