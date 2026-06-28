# ============================================================
# Development targets: tests, examples, benchmarks
# ============================================================

if(CORONET_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(CORONET_BUILD_TESTS)
    add_subdirectory(test)
endif()

if(CORONET_BUILD_BENCHMARKS)
    # bench/ directory created in Phase 4
    if(EXISTS "${PROJECT_SOURCE_DIR}/bench/CMakeLists.txt")
        add_subdirectory(bench)
    endif()
endif()
