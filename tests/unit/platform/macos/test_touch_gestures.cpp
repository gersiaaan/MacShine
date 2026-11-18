/**
 * @file tests/unit/platform/macos/test_touch_gestures.cpp
 * @brief Tests for turning touchscreen contacts into macOS pointer and scroll actions.
 */

#ifdef __APPLE__
  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <chrono>
  #include <vector>

  // local includes
  #include "src/platform/macos/touch_gestures.h"

using namespace platf::touch_gestures;
using namespace std::chrono_literals;

namespace {
  using clock_type = recognizer_t::clock_t;

  /**
   * @brief Recognizer with a controllable clock.
   */
  struct TouchGesturesTest: testing::Test {
    recognizer_t recognizer;  ///< Recognizer under test.
    clock_type::time_point now {clock_type::now()};  ///< Time of the next event.

    /**
     * @brief Advance the clock.
     * @param step Time to advance.
     */
    void wait(std::chrono::milliseconds step) {
      now += step;
    }
  };

  /**
   * @brief Check that an action is a button action.
   * @param action Action to check.
   * @param button Expected button.
   * @param down Expected press state.
   * @param clicks Expected click count.
   */
  void expect_button(const action_t &action, button_e button, bool down, int clicks) {
    EXPECT_EQ(action.kind, action_kind_e::button);
    EXPECT_EQ(action.button, button);
    EXPECT_EQ(action.down, down);
    EXPECT_EQ(action.clicks, clicks);
  }
}  // namespace

TEST_F(TouchGesturesTest, TapClicksWhereTheFingerTouched) {
  // The pointer goes under the finger right away, so hover effects see it before the click.
  const auto touched {recognizer.down(1, {100, 200}, now)};
  ASSERT_EQ(touched.size(), 1u);
  EXPECT_EQ(touched[0].kind, action_kind_e::move);
  EXPECT_EQ(touched[0].point.x, 100);
  EXPECT_EQ(touched[0].point.y, 200);
  wait(80ms);
  EXPECT_TRUE(recognizer.move(1, {103, 202}, now).empty());
  const auto actions {recognizer.up(1, {104, 203}, now)};
  ASSERT_EQ(actions.size(), 3u);
  EXPECT_EQ(actions[0].kind, action_kind_e::move);
  EXPECT_EQ(actions[0].point.x, 100);
  EXPECT_EQ(actions[0].point.y, 200);
  expect_button(actions[1], button_e::left, true, 1);
  expect_button(actions[2], button_e::left, false, 1);
}

TEST_F(TouchGesturesTest, QuickTapsDoubleAndTripleClick) {
  for (const int expected : {1, 2, 3, 3}) {
    recognizer.down(1, {100, 200}, now);
    wait(50ms);
    const auto actions {recognizer.up(1, {100, 200}, now)};
    ASSERT_EQ(actions.size(), 3u);
    expect_button(actions[1], button_e::left, true, expected);
    wait(150ms);
  }
}

TEST_F(TouchGesturesTest, SlowOrDistantTapsStartOver) {
  recognizer.down(1, {100, 200}, now);
  recognizer.up(1, {100, 200}, now);
  wait(recognizer_t::MULTI_CLICK_TIME + 1ms);
  recognizer.down(1, {100, 200}, now);
  EXPECT_EQ(recognizer.up(1, {100, 200}, now)[1].clicks, 1);

  wait(50ms);
  recognizer.down(1, {300, 200}, now);
  EXPECT_EQ(recognizer.up(1, {300, 200}, now)[1].clicks, 1);
}

TEST_F(TouchGesturesTest, HoldingStillRightClicks) {
  recognizer.down(1, {100, 200}, now);
  wait(recognizer_t::LONG_PRESS);
  const auto actions {recognizer.up(1, {101, 200}, now)};
  ASSERT_EQ(actions.size(), 3u);
  expect_button(actions[1], button_e::right, true, 1);
  expect_button(actions[2], button_e::right, false, 1);
}

TEST_F(TouchGesturesTest, HoldingThenMovingDragsWithTheLeftButton) {
  recognizer.down(1, {100, 200}, now);
  wait(recognizer_t::LONG_PRESS);
  auto actions {recognizer.move(1, {150, 200}, now)};
  ASSERT_EQ(actions.size(), 3u);
  EXPECT_EQ(actions[0].kind, action_kind_e::move);
  EXPECT_EQ(actions[0].point.x, 100);
  expect_button(actions[1], button_e::left, true, 1);
  EXPECT_EQ(actions[2].point.x, 150);

  actions = recognizer.move(1, {180, 220}, now);
  ASSERT_EQ(actions.size(), 1u);
  EXPECT_EQ(actions[0].kind, action_kind_e::move);
  EXPECT_EQ(actions[0].point.y, 220);

  actions = recognizer.up(1, {190, 220}, now);
  ASSERT_EQ(actions.size(), 2u);
  EXPECT_EQ(actions[0].point.x, 190);
  expect_button(actions[1], button_e::left, false, 1);
}

TEST_F(TouchGesturesTest, MovingFromADragAreaDragsRightAway) {
  std::vector<point_t> checked;
  recognizer.set_drag_area([&](point_t point) {
    checked.push_back(point);
    return point.y < 50;
  });

  recognizer.down(1, {100, 20}, now);
  wait(16ms);
  auto actions {recognizer.move(1, {140, 20}, now)};
  ASSERT_EQ(actions.size(), 3u);
  EXPECT_EQ(actions[0].point.x, 100);
  expect_button(actions[1], button_e::left, true, 1);
  EXPECT_EQ(actions[2].point.x, 140);
  actions = recognizer.up(1, {150, 25}, now);
  ASSERT_EQ(actions.size(), 2u);
  expect_button(actions[1], button_e::left, false, 1);
  ASSERT_EQ(checked.size(), 1u);
  EXPECT_EQ(checked[0].y, 20);

  // Elsewhere the finger scrolls, and a tap is never checked.
  recognizer.down(1, {100, 300}, now);
  wait(16ms);
  actions = recognizer.move(1, {100, 340}, now);
  ASSERT_EQ(actions.size(), 2u);
  EXPECT_EQ(actions[1].kind, action_kind_e::scroll);
  recognizer.cancel();
  recognizer.down(1, {100, 20}, now);
  recognizer.up(1, {100, 20}, now);
  EXPECT_EQ(checked.size(), 2u);

  recognizer.set_drag_area({});
  recognizer.down(1, {100, 20}, now);
  wait(16ms);
  EXPECT_EQ(recognizer.move(1, {140, 20}, now)[1].kind, action_kind_e::scroll);
}

TEST_F(TouchGesturesTest, SwipingUpFromTheBottomEdgeShowsTheDock) {
  recognizer.set_bottom_edge(780.0);
  recognizer.down(1, {400, 770}, now);
  wait(16ms);
  auto actions {recognizer.move(1, {405, 740}, now)};
  ASSERT_EQ(actions.size(), 1u);
  EXPECT_EQ(actions[0].kind, action_kind_e::move);
  EXPECT_EQ(actions[0].point.x, 400);
  EXPECT_EQ(actions[0].point.y, 779);
  // The rest of the swipe does nothing, so the pointer stays at the edge.
  EXPECT_TRUE(recognizer.move(1, {410, 600}, now).empty());
  EXPECT_TRUE(recognizer.up(1, {410, 600}, now).empty());

  // Sideways or from higher up it scrolls.
  recognizer.down(1, {400, 770}, now);
  wait(16ms);
  EXPECT_EQ(recognizer.move(1, {440, 760}, now)[1].kind, action_kind_e::scroll);
  recognizer.cancel();
  recognizer.down(1, {400, 780 - recognizer_t::EDGE_ZONE - 10}, now);
  wait(16ms);
  EXPECT_EQ(recognizer.move(1, {400, 780 - recognizer_t::EDGE_ZONE - 50}, now)[1].kind, action_kind_e::scroll);
  recognizer.cancel();

  recognizer.set_bottom_edge(std::nullopt);
  recognizer.down(1, {400, 770}, now);
  wait(16ms);
  EXPECT_EQ(recognizer.move(1, {400, 740}, now)[1].kind, action_kind_e::scroll);
}

TEST_F(TouchGesturesTest, SmallWobblesStillTap) {
  recognizer.down(1, {100, 200}, now);
  EXPECT_TRUE(recognizer.move(1, {100 + recognizer_t::SLOP, 200}, now).empty());
  EXPECT_EQ(recognizer.up(1, {100 + recognizer_t::SLOP, 200}, now).size(), 3u);
}

TEST_F(TouchGesturesTest, MovingScrollsTheContentWithTheFinger) {
  recognizer.down(1, {100, 200}, now);
  wait(16ms);
  auto actions {recognizer.move(1, {100, 230}, now)};
  ASSERT_EQ(actions.size(), 2u);
  EXPECT_EQ(actions[0].kind, action_kind_e::move);
  EXPECT_EQ(actions[0].point.y, 200);
  EXPECT_EQ(actions[1].kind, action_kind_e::scroll);
  EXPECT_EQ(actions[1].phase, scroll_phase_e::began);
  EXPECT_EQ(actions[1].dy, 30);
  EXPECT_EQ(actions[1].dx, 0);

  wait(16ms);
  actions = recognizer.move(1, {95, 220}, now);
  ASSERT_EQ(actions.size(), 1u);
  EXPECT_EQ(actions[0].phase, scroll_phase_e::changed);
  EXPECT_EQ(actions[0].dy, -10);
  EXPECT_EQ(actions[0].dx, -5);

  EXPECT_TRUE(recognizer.move(1, {95, 220}, now).empty());

  // The finger rests before lifting, so nothing glides.
  wait(200ms);
  actions = recognizer.up(1, {95, 220}, now);
  ASSERT_EQ(actions.size(), 1u);
  EXPECT_EQ(actions[0].phase, scroll_phase_e::ended);
  EXPECT_FALSE(recognizer.gliding());
  EXPECT_FALSE(recognizer.tick(now));
}

TEST_F(TouchGesturesTest, FlickGlidesAndSlowsDown) {
  recognizer.down(1, {100, 500}, now);
  for (int step = 1; step <= 5; ++step) {
    wait(10ms);
    recognizer.move(1, {100, 500.0 - step * 20}, now);
  }
  const auto actions {recognizer.up(1, {100, 400}, now)};
  ASSERT_EQ(actions.size(), 1u);
  EXPECT_EQ(actions[0].phase, scroll_phase_e::ended);
  ASSERT_TRUE(recognizer.gliding());

  wait(8ms);
  auto step {recognizer.tick(now)};
  ASSERT_TRUE(step);
  EXPECT_EQ(step->phase, scroll_phase_e::momentum_began);
  EXPECT_LT(step->dy, 0);
  const double first {step->dy};

  wait(8ms);
  step = recognizer.tick(now);
  ASSERT_TRUE(step);
  EXPECT_EQ(step->phase, scroll_phase_e::momentum_changed);
  EXPECT_GT(step->dy, first);

  int steps {0};
  while (recognizer.gliding() && steps < 10000) {
    wait(8ms);
    step = recognizer.tick(now);
    ++steps;
  }
  ASSERT_TRUE(step);
  EXPECT_EQ(step->phase, scroll_phase_e::momentum_ended);
  EXPECT_FALSE(recognizer.tick(now));
}

TEST_F(TouchGesturesTest, TouchingStopsTheGlideWithoutClicking) {
  recognizer.down(1, {100, 500}, now);
  for (int step = 1; step <= 5; ++step) {
    wait(10ms);
    recognizer.move(1, {100, 500.0 - step * 20}, now);
  }
  recognizer.up(1, {100, 400}, now);
  wait(8ms);
  ASSERT_TRUE(recognizer.tick(now));

  auto actions {recognizer.down(2, {300, 300}, now)};
  ASSERT_EQ(actions.size(), 1u);
  EXPECT_EQ(actions[0].phase, scroll_phase_e::momentum_ended);
  EXPECT_FALSE(recognizer.gliding());
  EXPECT_TRUE(recognizer.move(2, {340, 300}, now).empty());
  EXPECT_TRUE(recognizer.up(2, {340, 300}, now).empty());

  // The next touch taps again.
  EXPECT_EQ(recognizer.down(3, {300, 300}, now).size(), 1u);
  EXPECT_EQ(recognizer.up(3, {300, 300}, now).size(), 3u);
}

TEST_F(TouchGesturesTest, GlideThatNeverStartedEndsQuietly) {
  recognizer.down(1, {100, 500}, now);
  wait(10ms);
  recognizer.move(1, {100, 488}, now);
  wait(10ms);
  recognizer.move(1, {100, 476}, now);
  recognizer.up(1, {100, 476}, now);
  ASSERT_TRUE(recognizer.gliding());
  // A very long pause slows the glide below the stop speed before its first step.
  wait(10s);
  EXPECT_FALSE(recognizer.tick(now));
  EXPECT_FALSE(recognizer.gliding());
}

TEST_F(TouchGesturesTest, TwoFingerTapRightClicks) {
  recognizer.down(1, {100, 200}, now);
  recognizer.down(2, {160, 200}, now);
  wait(60ms);
  auto actions {recognizer.up(2, {160, 200}, now)};
  ASSERT_EQ(actions.size(), 3u);
  EXPECT_EQ(actions[0].point.x, 100);
  expect_button(actions[1], button_e::right, true, 1);
  EXPECT_TRUE(recognizer.up(1, {100, 200}, now).empty());
}

TEST_F(TouchGesturesTest, TwoFingersScrollByTheirMiddle) {
  recognizer.down(1, {100, 200}, now);
  recognizer.down(2, {200, 200}, now);
  wait(16ms);
  EXPECT_TRUE(recognizer.move(1, {100, 205}, now).empty());
  auto actions {recognizer.move(2, {200, 225}, now)};
  ASSERT_EQ(actions.size(), 2u);
  EXPECT_EQ(actions[0].kind, action_kind_e::move);
  EXPECT_EQ(actions[0].point.x, 150);
  EXPECT_EQ(actions[1].phase, scroll_phase_e::began);
  EXPECT_EQ(actions[1].dy, 15);

  wait(16ms);
  actions = recognizer.move(1, {100, 225}, now);
  ASSERT_EQ(actions.size(), 1u);
  EXPECT_EQ(actions[0].phase, scroll_phase_e::changed);
  EXPECT_EQ(actions[0].dy, 10);

  wait(200ms);
  actions = recognizer.up(1, {100, 225}, now);
  ASSERT_EQ(actions.size(), 1u);
  EXPECT_EQ(actions[0].phase, scroll_phase_e::ended);
  EXPECT_TRUE(recognizer.move(2, {200, 300}, now).empty());
  EXPECT_TRUE(recognizer.up(2, {200, 300}, now).empty());
}

TEST_F(TouchGesturesTest, SecondFingerJoinsAScrollWithoutAJump) {
  recognizer.down(1, {100, 200}, now);
  wait(16ms);
  recognizer.move(1, {100, 240}, now);
  EXPECT_TRUE(recognizer.down(2, {300, 240}, now).empty());
  wait(16ms);
  const auto actions {recognizer.move(2, {300, 260}, now)};
  ASSERT_EQ(actions.size(), 1u);
  EXPECT_EQ(actions[0].phase, scroll_phase_e::changed);
  EXPECT_EQ(actions[0].dy, 10);
}

TEST_F(TouchGesturesTest, ExtraAndUnknownFingersAreIgnored) {
  EXPECT_TRUE(recognizer.move(9, {1, 1}, now).empty());
  EXPECT_TRUE(recognizer.up(9, {1, 1}, now).empty());

  recognizer.down(1, {100, 200}, now);
  EXPECT_TRUE(recognizer.down(1, {100, 200}, now).empty());
  recognizer.down(2, {150, 200}, now);
  EXPECT_TRUE(recognizer.down(3, {200, 200}, now).empty());
  EXPECT_TRUE(recognizer.move(3, {260, 260}, now).empty());
  EXPECT_TRUE(recognizer.up(3, {260, 260}, now).empty());
}

TEST_F(TouchGesturesTest, CancelReleasesTheDragAndEndsScrolling) {
  recognizer.down(1, {100, 200}, now);
  wait(recognizer_t::LONG_PRESS);
  recognizer.move(1, {150, 200}, now);
  auto actions {recognizer.cancel()};
  ASSERT_EQ(actions.size(), 1u);
  expect_button(actions[0], button_e::left, false, 1);

  recognizer.down(1, {100, 200}, now);
  wait(16ms);
  recognizer.move(1, {100, 260}, now);
  actions = recognizer.cancel();
  ASSERT_EQ(actions.size(), 1u);
  EXPECT_EQ(actions[0].phase, scroll_phase_e::ended);
  EXPECT_FALSE(recognizer.gliding());

  recognizer.down(1, {100, 200}, now);
  EXPECT_TRUE(recognizer.cancel().empty());
  EXPECT_TRUE(recognizer.up(1, {100, 200}, now).empty());
}

TEST_F(TouchGesturesTest, CancelStopsAGlide) {
  recognizer.down(1, {100, 500}, now);
  for (int step = 1; step <= 5; ++step) {
    wait(10ms);
    recognizer.move(1, {100, 500.0 - step * 20}, now);
  }
  recognizer.up(1, {100, 400}, now);
  wait(8ms);
  ASSERT_TRUE(recognizer.tick(now));
  const auto actions {recognizer.cancel()};
  ASSERT_EQ(actions.size(), 1u);
  EXPECT_EQ(actions[0].phase, scroll_phase_e::momentum_ended);
}

#endif
