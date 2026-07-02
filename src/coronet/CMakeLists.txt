# ============================================================
# Library sources for coronet
# ============================================================
#
# Platform-specific code is in platform/<os>/ directories.
# Cross-platform code has #ifdef guards where needed.

file(GLOB_RECURSE coronet_common_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# Exclude all platform-specific directories
list(FILTER coronet_common_sources EXCLUDE REGEX "platform/iocp/")
list(FILTER coronet_common_sources EXCLUDE REGEX "platform/io_uring/")
list(FILTER coronet_common_sources EXCLUDE REGEX "platform/epoll/")

# Add platform-specific sources
if(WIN32)
    file(GLOB_RECURSE coronet_platform_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/platform/iocp/*.cpp")
elseif(CORONET_IOURING)
    file(GLOB_RECURSE coronet_platform_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/platform/io_uring/*.cpp")
else()
    file(GLOB_RECURSE coronet_platform_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/platform/epoll/*.cpp")
endif()
list(APPEND coronet_common_sources ${coronet_platform_sources})

target_sources(coronet PRIVATE ${coronet_common_sources})
