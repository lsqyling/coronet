# ============================================================
# Development targets: tests, examples, benchmarks
# (CORONET_DEVELOPER_MODE logic is in Option.cmake, runs before Extra.cmake)
# ============================================================
if(CORONET_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(CORONET_BUILD_TESTS)
    add_subdirectory(test)
endif()

if(CORONET_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()

if(CORONET_BUILD_STRESS_TESTS)
    add_subdirectory(stress-test)
endif()
