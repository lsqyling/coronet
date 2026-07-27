# ============================================================
# CTest infrastructure — enable_testing, test wrapper, cleanup
# ============================================================
# 职责：CTest 基础设施管理
#   - enable_testing()
#   - coronet_add_test() 包装函数（自动绑定 cleanup fixture）
#   - cleanup fixture（所有测试完成后自动运行 cleanup.py）
#   - 手动 cleanup target
#   - 脚本复制到 build 目录
#
# 必须在 Develop.cmake 之前 include，使 coronet_add_test 函数
# 在 test/、bench/、stress-test/ 子目录中可用。
# ============================================================

if(CORONET_BUILD_TESTS OR CORONET_BUILD_STRESS_TESTS OR CORONET_BUILD_BENCHMARKS)
    enable_testing()
endif()

# ---- Python3 for cleanup script ----
find_package(Python3 QUIET COMPONENTS Interpreter)

# ---- coronet_add_test: wrapper that binds cleanup fixture ----
# Usage: coronet_add_test(name COMMAND <cmd> [args...])
# All tests registered through this wrapper automatically require the
# "coronet_env" fixture, ensuring the cleanup test runs after them.
function(coronet_add_test name)
    add_test(NAME ${name} ${ARGN})
    set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED "coronet_env")
endfunction()

# ---- Cleanup fixture — runs after ALL tests complete ----
# FIXTURES_CLEANUP guarantees execution after every test with
# FIXTURES_REQUIRED "coronet_env", even if some tests fail.
if(Python3_FOUND)
    add_test(NAME coronet_cleanup
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/script/cleanup.py
                --build-dir ${CMAKE_BINARY_DIR}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
else()
    add_test(NAME coronet_cleanup
        COMMAND ${CMAKE_COMMAND} -E echo "Python3 not found, cleanup skipped")
endif()
set_tests_properties(coronet_cleanup PROPERTIES
    FIXTURES_CLEANUP "coronet_env"
    LABELS "cleanup")

# ---- Manual cleanup target ----
if(Python3_FOUND)
    add_custom_target(cleanup
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/script/cleanup.py
                --build-dir ${CMAKE_BINARY_DIR}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Running test cleanup script")
endif()

# ============================================================
# Copy test/stress scripts to build directory (developer mode)
# Makes bench_c1000k.sh/.ps1, cleanup.py, etc. available next to
# the built binaries for convenient in-tree testing.
# ============================================================
if(CORONET_DEVELOPER_MODE AND EXISTS "${PROJECT_SOURCE_DIR}/script")
    add_custom_target(copy_test_scripts ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/script"
            "${CMAKE_BINARY_DIR}/script"
        COMMENT "Copying test scripts to build directory"
    )
endif()
