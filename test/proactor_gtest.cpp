// gtest-based proactor tests.
// These verify the platform proactor can init/deinit, allocate operations,
// submit them, and process completions.
// On Linux this exercises actual io_uring; on Windows it exercises IOCP.

#include <gtest/gtest.h>

#include "coronet/platform/proactor.hpp"

#if defined(CORONET_PLATFORM_LINUX)
#include "coronet/platform/io_uring/io_uring_proactor.hpp"
using Proactor = coronet::platform::io_uring::io_uring_proactor;

#elif defined(CORONET_PLATFORM_WINDOWS)
#include "coronet/platform/iocp/iocp_proactor.hpp"
using Proactor = coronet::platform::iocp::iocp_proactor;

#endif

TEST(ProactorTest, InitAndDeinit) {
    Proactor p;
    EXPECT_NO_THROW(p.init(64));
    p.deinit();
}

TEST(ProactorTest, DoubleInitIsSafe) {
    Proactor p;
    p.init(64);
    p.init(128);
    p.deinit();
}

TEST(ProactorTest, DeinitWithoutInit) {
    Proactor p;
    EXPECT_NO_THROW(p.deinit());
}

TEST(ProactorTest, AllocateOperation) {
    Proactor p;
    p.init(64);
    auto op = p.acquire_operation();
    if (op) {
        op->set_user_data(42);
    }
    p.deinit();
}

#if defined(CORONET_PLATFORM_LINUX)

TEST(ProactorTest, SubmitWithNoOps) {
    Proactor p;
    p.init(64);
    auto* sqe = p.get_sq_entry();
    ASSERT_NE(sqe, nullptr);
    sqe->prep_nop();
    sqe->set_data(0x1234);
    int ret = p.submit(false);
    EXPECT_GE(ret, 0);
    coronet::platform::completion_info info;
    ret = p.wait_completion(&info);
    EXPECT_EQ(ret, 1);
    EXPECT_EQ(info.user_data, 0x1234);
    p.deinit();
}

TEST(ProactorTest, NativeHandleIsValid) {
    Proactor p;
    p.init(64);
    intptr_t fd = p.native_handle();
    EXPECT_GE(fd, 0);
    p.deinit();
}

#endif
