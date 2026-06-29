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
    add_subdirectory(bench)
endif()

if(CORONET_BUILD_STRESS_TESTS)
    add_subdirectory(stress-test)
endif()
