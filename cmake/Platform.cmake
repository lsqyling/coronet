# ============================================================
# Platform-specific settings for coronet
# ============================================================

if(WIN32)
    # Windows: IOCP proactor
    target_compile_definitions(coronet PUBLIC CORONET_PLATFORM_WINDOWS)
    target_link_libraries(coronet PUBLIC
        ws2_32
        kernel32
        mswsock)

elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Linux: epoll proactor (default) or io_uring (CORONET_IOURING=ON)
    target_compile_definitions(coronet PUBLIC CORONET_PLATFORM_LINUX)
    if(CORONET_IOURING)
        target_compile_definitions(coronet PUBLIC CORONET_USE_IOURING)
    endif()

    # Threads (required for std::thread)
    find_package(Threads REQUIRED)
    target_link_libraries(coronet PUBLIC Threads::Threads)

    # Optional: raw liburing for liburing_tests
    find_package(LibUring QUIET)

else()
    message(FATAL_ERROR "coronet: unsupported platform ${CMAKE_SYSTEM_NAME}")
endif()
