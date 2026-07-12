# ============================================================
# Development targets: tests, examples, benchmarks
# ============================================================
if (CORONET_DEVELOPER_MODE)
    set(CORONET_BUILD_EXAMPLES ON)
    set(CORONET_BUILD_TESTS ON)
    set(CORONET_BUILD_BENCHMARKS ON)
    set(CORONET_BUILD_STRESS_TESTS ON)
message(NOTICE "CORONET_DEVELOPER_MODE: ${CORONET_DEVELOPER_MODE},it means build all targets")
endif ()
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
