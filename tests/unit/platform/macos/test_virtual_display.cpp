/**
 * @file tests/unit/platform/macos/test_virtual_display.cpp
 * @brief Tests for the macOS virtual display helper interface.
 */

#ifdef __APPLE__
  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <string>
  #include <vector>

  // local includes
  #include "src/platform/macos/virtual_display.h"

TEST(MacosVirtualDisplayTest, HelperArgumentsCarryEveryRequestField) {
  const platf::virtual_display::request_t request {
    .width = 2388,
    .height = 1688,
    .fps = 120,
    .hidpi = true,
    .make_primary = false,
    .disable_others = true,
    .serial = 42,
    .name = "MacShine - iPad",
  };

  const std::vector<std::string> expected {
    "/helper",
    "--width",
    "2388",
    "--height",
    "1688",
    "--fps",
    "120",
    "--hidpi",
    "1",
    "--primary",
    "0",
    "--disable-others",
    "1",
    "--serial",
    "42",
    "--name",
    "MacShine - iPad",
  };
  EXPECT_EQ(platf::virtual_display::helper_arguments("/helper", request), expected);
}

TEST(MacosVirtualDisplayTest, HelperArgumentsKeepHostDisplaysByDefault) {
  const platf::virtual_display::request_t request {
    .width = 1920,
    .height = 1080,
    .fps = 60,
    .hidpi = false,
    .make_primary = true,
    .disable_others = false,
    .serial = 1,
    .name = "MacShine",
  };

  const auto args {platf::virtual_display::helper_arguments("/helper", request)};
  ASSERT_EQ(args.size(), 17u);
  EXPECT_EQ(args[8], "0");  // --hidpi
  EXPECT_EQ(args[10], "1");  // --primary
  EXPECT_EQ(args[11], "--disable-others");
  EXPECT_EQ(args[12], "0");
}

TEST(MacosVirtualDisplayTest, SerialForClientIsStableAndNonZero) {
  EXPECT_EQ(platf::virtual_display::serial_for_client("client-a"), platf::virtual_display::serial_for_client("client-a"));
  EXPECT_NE(platf::virtual_display::serial_for_client("client-a"), platf::virtual_display::serial_for_client("client-b"));
  EXPECT_NE(platf::virtual_display::serial_for_client(""), 0u);
}
#endif
