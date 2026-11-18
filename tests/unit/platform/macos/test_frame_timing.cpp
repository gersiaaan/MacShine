/**
 * @file tests/unit/platform/macos/test_frame_timing.cpp
 * @brief Tests for converting macOS capture times to Sunshine frame timestamps.
 */

#ifdef __APPLE__
  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <chrono>

  // local includes
  #include "src/platform/macos/frame_timing.h"

using namespace std::chrono_literals;
using namespace platf::frame_timing;

namespace {
  CMTime host_time_offset(std::chrono::milliseconds offset) {
    return CMTimeAdd(CMClockGetTime(CMClockGetHostTimeClock()), CMTimeMake(offset.count(), 1000));
  }
}  // namespace

TEST(MacosFrameTimingTest, HostTimeMatchesSteadyClock) {
  const auto before {std::chrono::steady_clock::now()};
  const auto converted {host_time_to_steady(CMClockGetTime(CMClockGetHostTimeClock()))};
  const auto after {std::chrono::steady_clock::now()};

  ASSERT_TRUE(converted.has_value());
  EXPECT_GE(*converted, before - 1ms);
  EXPECT_LE(*converted, after + 1ms);
}

TEST(MacosFrameTimingTest, InvalidHostTimeHasNoSteadyTime) {
  EXPECT_FALSE(host_time_to_steady(kCMTimeInvalid).has_value());
}

TEST(MacosFrameTimingTest, RecentFramesKeepTheirCaptureTime) {
  const auto presentation {host_time_offset(-30ms)};
  const auto now {std::chrono::steady_clock::now()};
  const auto stamp {capture_timestamp(presentation, now)};

  EXPECT_LT(stamp, now - 20ms);
  EXPECT_GT(stamp, now - 50ms);
}

TEST(MacosFrameTimingTest, StaleFutureAndInvalidFramesUseDeliveryTime) {
  const auto now {std::chrono::steady_clock::now()};

  EXPECT_EQ(capture_timestamp(host_time_offset(-1000ms), now), now);
  EXPECT_EQ(capture_timestamp(host_time_offset(1000ms), now), now);
  EXPECT_EQ(capture_timestamp(kCMTimeInvalid, now), now);
}

TEST(MacosFrameTimingTest, MinFrameIntervalIsNinetyPercentOfTheFrameTime) {
  EXPECT_EQ(CMTimeCompare(macshine_capture_min_frame_interval(60), CMTimeMake(15, 1000)), 0);
  EXPECT_EQ(CMTimeCompare(macshine_capture_min_frame_interval(120), CMTimeMake(75, 10000)), 0);
}

TEST(MacosFrameTimingTest, MinFrameIntervalStaysBelowTwoRefreshesOfTheDisplay) {
  // The virtual display runs at twice the stream rate; two refreshes are one frame time.
  for (const int rate : {30, 60, 120}) {
    const CMTime two_refreshes {CMTimeMake(2, 2 * rate)};
    EXPECT_LT(CMTimeCompare(macshine_capture_min_frame_interval(rate), two_refreshes), 0) << rate;
  }
}

TEST(MacosFrameTimingTest, MinFrameIntervalDefaultsToSixtyFps) {
  EXPECT_EQ(CMTimeCompare(macshine_capture_min_frame_interval(0), macshine_capture_min_frame_interval(60)), 0);
  EXPECT_EQ(CMTimeCompare(macshine_capture_min_frame_interval(-5), macshine_capture_min_frame_interval(60)), 0);
}

#endif
