# ============================================================
# Platform-specific settings for coronet
# ============================================================

if(WIN32)
    # Windows: IOCP proactor
    target_compile_definitions(coronet PUBLIC CORONET_PLATFORM_WINDOWS)
    target_link_libraries(coronet PUBLIC
        ws2_32
        kernel32
        mswsock
        winmm)  # timeBeginPeriod — 定时器精度 15.625ms → 1ms

elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    # Linux / HarmonyOS: both use the Linux kernel, so epoll / eventfd / timerfd
    # are all available.  HarmonyOS (OHOS) toolchains may report a different
    # CMAKE_SYSTEM_NAME, but __linux__ is defined either way.
    #
    # Proactor: epoll (default) or io_uring (CORONET_IOURING=ON)
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
    message(FATAL_ERROR
        "coronet: unsupported platform '${CMAKE_SYSTEM_NAME}'.\n"
        "  Supported: Windows (IOCP), Linux/HarmonyOS (epoll / io_uring).\n"
        "  macOS is not yet supported — kqueue proactor is needed.")
endif()
