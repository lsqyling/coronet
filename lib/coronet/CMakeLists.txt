# ============================================================
# Library sources for coronet
# ============================================================
#
# Platform-specific code is in platform/<os>/ directories.
# Cross-platform code has #ifdef guards where needed.

file(GLOB_RECURSE coronet_common_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# Exclude platform-specific directories
list(FILTER coronet_common_sources EXCLUDE REGEX "platform/iocp/")
list(FILTER coronet_common_sources EXCLUDE REGEX "platform/io_uring/")

# Add platform-specific sources
if(WIN32)
    file(GLOB_RECURSE coronet_win_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/platform/iocp/*.cpp")
    list(APPEND coronet_common_sources ${coronet_win_sources})
else()
    file(GLOB_RECURSE coronet_linux_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/platform/io_uring/*.cpp")
    list(APPEND coronet_common_sources ${coronet_linux_sources})
endif()

target_sources(coronet PRIVATE ${coronet_common_sources})
