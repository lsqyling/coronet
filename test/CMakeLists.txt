# ============================================================
# Test targets for coronet — CTest integration
# ============================================================

# ---- GoogleTest unit tests ----
set(coronet_gtest_tests
    task_gtest
    generator_gtest
    channel_gtest
    shared_task_gtest
)

foreach(test_target ${coronet_gtest_tests})
    add_executable(${test_target} ${test_target}.cpp)
    target_link_libraries(${test_target} PRIVATE coronet::coronet gtest gtest_main)
    add_test(NAME ${test_target} COMMAND $<TARGET_FILE:${test_target}>)
endforeach()

# ---- Standalone integration tests (co_spawn + assert) ----
set(coronet_int_tests
    ft_task
    move_shared_task
    generator_test
    coro_lifetime
    mutex
    sem
    timer
    timer_accuracy
    when_all
    when_any
    when_some
    channel
    cv_notify_all
    cv_notify_one
)

# stress_test uses POSIX sockets (sys/socket.h) — Linux only
if(NOT WIN32)
    list(APPEND coronet_int_tests stress_test)
endif()

foreach(test_target ${coronet_int_tests})
    add_executable(${test_target} ${test_target}.cpp)
    target_link_libraries(${test_target} PRIVATE coronet::coronet)
    add_test(NAME ${test_target} COMMAND $<TARGET_FILE:${test_target}>)
endforeach()
