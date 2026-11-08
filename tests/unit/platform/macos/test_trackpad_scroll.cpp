/**
 * @file tests/unit/platform/macos/test_trackpad_scroll.cpp
 * @brief Tests for posting client scrolling as smooth trackpad scroll gestures on macOS.
 */

#ifdef __APPLE__
  // test includes
  #include "../../../tests_common.h"

  // local includes
  #include "src/platform/macos/trackpad_scroll.h"

using platf::trackpad_scroll::gesture_t;
using platf::trackpad_scroll::phase_e;

TEST(MacosTrackpadScrollTest, BurstsAreSpreadOverTicks) {
  gesture_t gesture;
  EXPECT_FALSE(gesture.active());
  EXPECT_FALSE(gesture.tick(true).has_value());

  // Three packets in the same millisecond: 3 x 120 units = 150 pixels.
  gesture.add(120, 0);
  gesture.add(120, 0);
  gesture.add(120, 0);

  auto event {gesture.tick(false)};
  ASSERT_TRUE(event.has_value());
  EXPECT_EQ(event->phase, phase_e::began);
  EXPECT_EQ(event->vertical, 75);
  EXPECT_TRUE(gesture.active());

  int total {event->vertical};
  int ticks {1};
  while (gesture.pending()) {
    event = gesture.tick(false);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->phase, phase_e::changed);
    EXPECT_GT(event->vertical, 0);
    total += event->vertical;
    ++ticks;
  }
  EXPECT_EQ(total, 150);
  EXPECT_GT(ticks, 5);  // A glide, not one jump.

  // Nothing left: no event until the gesture is idle, then the end.
  EXPECT_FALSE(gesture.tick(false).has_value());
  event = gesture.tick(true);
  ASSERT_TRUE(event.has_value());
  EXPECT_EQ(event->phase, phase_e::ended);
  EXPECT_EQ(event->vertical, 0);
  EXPECT_EQ(event->horizontal, 0);
  EXPECT_FALSE(gesture.active());
  EXPECT_FALSE(gesture.tick(true).has_value());
}

TEST(MacosTrackpadScrollTest, SmallPacketsAddUpInsteadOfBeingLost) {
  gesture_t gesture;

  // 3 units are 1.25 pixels: four of them make 5 pixels.
  int total {0};
  for (int packet = 0; packet < 4; ++packet) {
    gesture.add(-3, 0);
    while (const auto event {gesture.tick(false)}) {
      total += event->vertical;
    }
  }
  EXPECT_EQ(total, -5);
}

TEST(MacosTrackpadScrollTest, HorizontalAndVerticalMoveTogether) {
  gesture_t gesture;
  gesture.add(0, -240);
  const auto event {gesture.tick(false)};
  ASSERT_TRUE(event.has_value());
  EXPECT_EQ(event->vertical, 0);
  EXPECT_EQ(event->horizontal, -50);
}

TEST(MacosTrackpadScrollTest, EndingDropsLeftoverFractions) {
  gesture_t gesture;
  gesture.add(3, 0);  // 1.25 pixels
  ASSERT_TRUE(gesture.tick(false).has_value());  // posts 1, leaves 0.25
  ASSERT_TRUE(gesture.tick(true).has_value());  // ends the gesture
  gesture.add(2, 0);  // 0.83 pixels, without the old 0.25
  EXPECT_FALSE(gesture.tick(false).has_value());
}

TEST(MacosTrackpadScrollTest, TestBuildsStartWithGesturesOff) {
  EXPECT_FALSE(platf::trackpad_scroll::enabled());
  platf::trackpad_scroll::set_enabled(true);
  EXPECT_TRUE(platf::trackpad_scroll::enabled());
  platf::trackpad_scroll::set_enabled(false);
  EXPECT_FALSE(platf::trackpad_scroll::enabled());
}

#endif
