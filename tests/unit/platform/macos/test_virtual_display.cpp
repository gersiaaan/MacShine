/**
 * @file tests/unit/platform/macos/test_virtual_display.cpp
 * @brief Tests for the macOS virtual display helper interface.
 */

#ifdef __APPLE__
  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <string>
  #include <utility>
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
    .scale_percent = 100,
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
    "--scale",
    "100",
    "--sizes",
    "75:1790x1266,85:2030x1434,90:2148x1518,100:2388x1688,115:2746x1940,130:3104x2194,150:3582x2532,175:4178x2954,200:4776x3376",
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
    .scale_percent = 100,
    .serial = 1,
    .name = "MacShine",
  };

  const auto args {platf::virtual_display::helper_arguments("/helper", request)};
  ASSERT_EQ(args.size(), 21u);
  EXPECT_EQ(args[8], "0");  // --hidpi
  EXPECT_EQ(args[10], "1");  // --primary
  EXPECT_EQ(args[11], "--disable-others");
  EXPECT_EQ(args[12], "0");
}

TEST(MacosVirtualDisplayTest, DisplayPixelSizeScalesTheClientResolution) {
  platf::virtual_display::request_t request {
    .width = 2388,
    .height = 1668,
    .fps = 60,
    .hidpi = true,
    .make_primary = false,
    .disable_others = false,
    .scale_percent = 100,
    .serial = 1,
    .name = "MacShine",
  };
  EXPECT_EQ(platf::virtual_display::display_pixel_size(request), std::make_pair(2388, 1668));

  request.scale_percent = 150;
  EXPECT_EQ(platf::virtual_display::display_pixel_size(request), std::make_pair(3582, 2502));

  // HiDPI sizes stay even, because the desktop is half of them in points.
  request.scale_percent = 85;
  EXPECT_EQ(platf::virtual_display::display_pixel_size(request), std::make_pair(2030, 1418));

  request.hidpi = false;
  EXPECT_EQ(platf::virtual_display::display_pixel_size(request), std::make_pair(2030, 1418));
  request.scale_percent = 115;
  EXPECT_EQ(platf::virtual_display::display_pixel_size(request), std::make_pair(2746, 1918));
}

TEST(MacosVirtualDisplayTest, DisplayPixelSizeStaysWithinHelperLimits) {
  platf::virtual_display::request_t request {
    .width = 1920,
    .height = 1080,
    .fps = 60,
    .hidpi = true,
    .make_primary = false,
    .disable_others = false,
    .scale_percent = 10,
    .serial = 1,
    .name = "MacShine",
  };
  EXPECT_EQ(platf::virtual_display::display_pixel_size(request), std::make_pair(960, 540));

  request.scale_percent = 500;
  EXPECT_EQ(platf::virtual_display::display_pixel_size(request), std::make_pair(3840, 2160));

  // A 5K client at 200% would exceed the helper's 8192 pixel limit.
  request.width = 5120;
  request.height = 2880;
  request.scale_percent = 200;
  const auto [width, height] {platf::virtual_display::display_pixel_size(request)};
  EXPECT_LE(width, 8192);
  EXPECT_LE(height, 8192);
  EXPECT_GT(width, 5120);
}

TEST(MacosVirtualDisplayTest, HelperArgumentsUseTheScaledSize) {
  const platf::virtual_display::request_t request {
    .width = 2388,
    .height = 1668,
    .fps = 60,
    .hidpi = true,
    .make_primary = false,
    .disable_others = true,
    .scale_percent = 150,
    .serial = 7,
    .name = "MacShine",
  };
  const auto args {platf::virtual_display::helper_arguments("/helper", request)};
  EXPECT_EQ(args[2], "3582");
  EXPECT_EQ(args[4], "2502");
  EXPECT_EQ(args[13], "--scale");
  EXPECT_EQ(args[14], "150");
}

TEST(MacosVirtualDisplayTest, SizeOptionsOfferEveryPresetAroundTheConfiguredSize) {
  platf::virtual_display::request_t request {
    .width = 1800,
    .height = 1350,
    .fps = 60,
    .hidpi = true,
    .make_primary = false,
    .disable_others = true,
    .scale_percent = 100,
    .serial = 1,
    .name = "MacShine",
  };

  const auto options {platf::virtual_display::size_options(request)};
  ASSERT_EQ(options.size(), platf::virtual_display::SIZE_PRESETS.size());
  EXPECT_EQ(options.front(), (platf::virtual_display::size_option_t {75, 1350, 1012}));
  EXPECT_EQ(options[2], (platf::virtual_display::size_option_t {90, 1620, 1214}));
  EXPECT_EQ(options[3], (platf::virtual_display::size_option_t {100, 1800, 1350}));
  EXPECT_EQ(options.back(), (platf::virtual_display::size_option_t {200, 3600, 2700}));

  // A configured size between the presets is offered too, in order.
  request.scale_percent = 95;
  const auto with_custom {platf::virtual_display::size_options(request)};
  ASSERT_EQ(with_custom.size(), platf::virtual_display::SIZE_PRESETS.size() + 1);
  EXPECT_EQ(with_custom[3], (platf::virtual_display::size_option_t {95, 1710, 1282}));
}

TEST(MacosVirtualDisplayTest, SizeOptionsSkipSizesOutsideTheHelperLimits) {
  platf::virtual_display::request_t request {
    .width = 400,
    .height = 250,
    .fps = 60,
    .hidpi = false,
    .make_primary = false,
    .disable_others = false,
    .scale_percent = 50,
    .serial = 1,
    .name = "MacShine",
  };

  // The configured size stays even when it is too small; smaller presets are dropped.
  auto options {platf::virtual_display::size_options(request)};
  EXPECT_EQ(options.front(), (platf::virtual_display::size_option_t {50, 200, 125}));
  EXPECT_EQ(options[1].percent, 85);

  // Presets capped at the helper's 8192 pixel limit collapse into one option.
  request.width = 5120;
  request.height = 2880;
  request.scale_percent = 100;
  options = platf::virtual_display::size_options(request);
  EXPECT_EQ(options.back().width, 8192);
  EXPECT_EQ(options.back().percent, 175);
}

TEST(MacosVirtualDisplayTest, SerialForClientIsStableAndNonZero) {
  EXPECT_EQ(platf::virtual_display::serial_for_client("client-a"), platf::virtual_display::serial_for_client("client-a"));
  EXPECT_NE(platf::virtual_display::serial_for_client("client-a"), platf::virtual_display::serial_for_client("client-b"));
  EXPECT_NE(platf::virtual_display::serial_for_client(""), 0u);
}

TEST(MacosVirtualDisplayTest, DisplayRunsAtTwiceTheStreamRate) {
  EXPECT_EQ(platf::virtual_display::display_refresh_rate(50), 100);
  EXPECT_EQ(platf::virtual_display::display_refresh_rate(30), 60);
  EXPECT_EQ(platf::virtual_display::display_refresh_rate(60), 120);
  EXPECT_EQ(platf::virtual_display::display_refresh_rate(90), 120);
  EXPECT_EQ(platf::virtual_display::display_refresh_rate(120), 120);
  EXPECT_EQ(platf::virtual_display::display_refresh_rate(144), 144);
  EXPECT_EQ(platf::virtual_display::display_refresh_rate(0), 0);
}

TEST(MacosVirtualDisplayTest, HelperArgumentsDoubleTheStreamRate) {
  const platf::virtual_display::request_t request {
    .width = 1920,
    .height = 1200,
    .fps = 50,
    .hidpi = true,
    .make_primary = false,
    .disable_others = true,
    .scale_percent = 100,
    .serial = 7,
    .name = "MacShine",
  };

  const auto args {platf::virtual_display::helper_arguments("/helper", request)};
  ASSERT_GE(args.size(), 7u);
  EXPECT_EQ(args[5], "--fps");
  EXPECT_EQ(args[6], "100");
}

TEST(MacosVirtualDisplayTest, WakeForStreamDeclaresUserActivity) {
  EXPECT_TRUE(platf::virtual_display::wake_for_stream());
}

TEST(MacosVirtualDisplayTest, SizeMessageCarriesPixelsAndPoints) {
  EXPECT_EQ(platf::virtual_display::size_message(2496, 1560, 1248, 780), "SIZE 2496 1560 1248 780\n");
}

TEST(MacosVirtualDisplayTest, ReportSizeWithoutActiveDisplayIsIgnored) {
  platf::virtual_display::destroy();
  EXPECT_TRUE(platf::virtual_display::active_display_id().empty());

  // Neither an unknown display nor invalid sizes may reach a helper or fail.
  platf::virtual_display::report_size("119", 2496, 1560, 1248, 780);
  platf::virtual_display::report_size("", 2496, 1560, 1248, 780);
  platf::virtual_display::report_size("119", 0, 1560, 1248, 780);
  platf::virtual_display::report_size("119", 2496, 1560, 0, 780);
  EXPECT_TRUE(platf::virtual_display::active_display_id().empty());
}
#endif
