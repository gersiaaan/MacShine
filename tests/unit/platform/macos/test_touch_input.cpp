/**
 * @file tests/unit/platform/macos/test_touch_input.cpp
 * @brief Tests for handling touchscreen and pen input from clients on macOS.
 */

#ifdef __APPLE__
  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <vector>

  // local includes
  #include "src/config.h"
  #include "src/platform/common.h"
  #include "src/platform/macos/touch_input.h"

namespace {
  /**
   * @brief Geometry of a 1248x780 point display at the desktop origin.
   * @return Touch port.
   */
  platf::touch_port_t display_port() {
    return {0, 0, 1248, 780, 1248, 780, 0, 0};
  }

  /**
   * @brief Build a touch event.
   * @param type Moonlight touch event type.
   * @param x Horizontal position as a fraction of the display width.
   * @param y Vertical position as a fraction of the display height.
   * @return Touch event.
   */
  platf::touch_input_t touch_event(std::uint8_t type, float x, float y) {
    return {type, LI_ROT_UNKNOWN, 1, x, y, 1.0f, 0.0f, 0.0f};
  }

  /**
   * @brief Build a pen event.
   * @param type Moonlight touch event type.
   * @param buttons Pen button mask.
   * @return Pen event.
   */
  platf::pen_input_t pen_event(std::uint8_t type, std::uint8_t buttons) {
    return {type, LI_TOOL_TYPE_PEN, buttons, LI_TILT_UNKNOWN, LI_ROT_UNKNOWN, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f};
  }
}  // namespace

TEST(MacosTouchInputTest, TestBuildsDoNotPostEvents) {
  EXPECT_FALSE(platf::touch_input::enabled());
  platf::touch_input::set_enabled(true);
  EXPECT_TRUE(platf::touch_input::enabled());
  platf::touch_input::set_enabled(false);
}

TEST(MacosTouchInputTest, TouchPositionsAreFractionsOfTheStreamedDisplay) {
  const platf::touch_port_t port {100, 50, 1248, 780, 1248, 780};
  auto point {platf::touch_input::to_display(port, 0.5f, 0.25f)};
  EXPECT_DOUBLE_EQ(point.x, 100.0 + 624.0);
  EXPECT_DOUBLE_EQ(point.y, 50.0 + 195.0);
  point = platf::touch_input::to_display(port, 0.0f, 1.0f);
  EXPECT_DOUBLE_EQ(point.x, 100.0);
  EXPECT_DOUBLE_EQ(point.y, 830.0);
  // Positions outside the display are kept on it.
  point = platf::touch_input::to_display(port, -0.5f, 3.0f);
  EXPECT_DOUBLE_EQ(point.x, 100.0);
  EXPECT_DOUBLE_EQ(point.y, 830.0);
}

TEST(MacosTouchInputTest, TitleBarIsTheTopBandOfAWindow) {
  const CGRect window {{100, 30}, {800, 600}};
  EXPECT_TRUE(platf::touch_input::in_title_bar(window, {500, 30}));
  EXPECT_TRUE(platf::touch_input::in_title_bar(window, {100, 30 + platf::touch_input::TITLE_BAR_HEIGHT - 1}));
  EXPECT_FALSE(platf::touch_input::in_title_bar(window, {500, 30 + platf::touch_input::TITLE_BAR_HEIGHT}));
  EXPECT_FALSE(platf::touch_input::in_title_bar(window, {500, 29}));
  EXPECT_FALSE(platf::touch_input::in_title_bar(window, {99, 40}));
  EXPECT_FALSE(platf::touch_input::in_title_bar(window, {900, 40}));
}

TEST(MacosTouchInputTest, ControlsAndContentKeepTheWindowInPlace) {
  for (const auto role : {"AXButton", "AXTextField", "AXWebArea", "AXScrollArea", "AXRadioButton", "AXPopUpButton"}) {
    EXPECT_TRUE(platf::touch_input::role_blocks_window_drag(role)) << role;
  }
  for (const auto role : {"AXWindow", "AXToolbar", "AXGroup", "AXStaticText", "AXImage", ""}) {
    EXPECT_FALSE(platf::touch_input::role_blocks_window_drag(role)) << role;
  }
}

TEST(MacosTouchInputTest, TapsSnapToTheNearestSmallButton) {
  using platf::touch_input::choose_snap_target;
  const std::vector<CGRect> buttons {
    {{100, 40}, {16, 16}},  // close button, center 108,48
    {{140, 40}, {16, 16}},  // another one, center 148,48
    {{160, 30}, {28, 41}},  // new tab button, too tall to snap to
  };
  auto target {choose_snap_target({112, 58}, buttons)};
  ASSERT_TRUE(target);
  EXPECT_DOUBLE_EQ(target->x, 108);
  EXPECT_DOUBLE_EQ(target->y, 48);

  target = choose_snap_target({138, 50}, buttons);
  ASSERT_TRUE(target);
  EXPECT_DOUBLE_EQ(target->x, 148);

  // Right on a button, even a large one, the tap stays where it is.
  EXPECT_FALSE(choose_snap_target({162, 50}, buttons));
  EXPECT_FALSE(choose_snap_target({110, 45}, buttons));

  EXPECT_FALSE(choose_snap_target({100, 70}, buttons));
  EXPECT_FALSE(choose_snap_target({50, 50}, buttons));
  EXPECT_FALSE(choose_snap_target({50, 50}, {}));
  EXPECT_FALSE(choose_snap_target({108, 48}, {{{100, 40}, {0, 16}}}));
}

TEST(MacosTouchInputTest, NoSnapOrDockItemFarOffScreen) {
  EXPECT_FALSE(platf::touch_input::find_snap_target({-100000, -100000}));
  EXPECT_EQ(platf::touch_input::dock_item_at({-100000, -100000}), nullptr);
}

TEST(MacosTouchInputTest, NoWindowIsDraggedFarOffScreen) {
  EXPECT_FALSE(platf::touch_input::starts_window_drag({-100000, -100000}));
}

TEST(MacosTouchInputTest, TouchHidesThePointerAndMouseInputShowsIt) {
  platf::touch_input::mouse_used();
  EXPECT_FALSE(platf::touch_input::cursor_hidden());
  EXPECT_FALSE(macshine_stream_cursor_hidden());

  // Hovering does not count as touching.
  platf::touch_input::touch(display_port(), touch_event(LI_TOUCH_EVENT_HOVER, 0.1f, 0.1f));
  EXPECT_FALSE(platf::touch_input::cursor_hidden());

  platf::touch_update(nullptr, display_port(), touch_event(LI_TOUCH_EVENT_DOWN, 0.1f, 0.1f));
  EXPECT_TRUE(platf::touch_input::cursor_hidden());
  EXPECT_TRUE(macshine_stream_cursor_hidden());
  platf::touch_input::touch(display_port(), touch_event(LI_TOUCH_EVENT_MOVE, 0.2f, 0.1f));
  platf::touch_input::touch(display_port(), touch_event(LI_TOUCH_EVENT_UP, 0.2f, 0.1f));
  platf::touch_input::touch(display_port(), touch_event(LI_TOUCH_EVENT_CANCEL_ALL, 0, 0));
  platf::touch_input::touch(display_port(), touch_event(LI_TOUCH_EVENT_CANCEL, 0, 0));
  EXPECT_TRUE(platf::touch_input::cursor_hidden());

  platf::touch_input::mouse_used();
  EXPECT_FALSE(platf::touch_input::cursor_hidden());
}

TEST(MacosTouchInputTest, AbsoluteMouseMovesRightAfterATouchAreDropped) {
  platf::touch_input::touch(display_port(), touch_event(LI_TOUCH_EVENT_DOWN, 0.5f, 0.5f));
  platf::touch_input::touch(display_port(), touch_event(LI_TOUCH_EVENT_UP, 0.5f, 0.5f));
  EXPECT_FALSE(platf::touch_input::absolute_mouse_allowed());
  EXPECT_TRUE(platf::touch_input::cursor_hidden());
  platf::touch_input::forget_touches();
  EXPECT_TRUE(platf::touch_input::absolute_mouse_allowed());
  platf::touch_input::mouse_used("absolute move");
  EXPECT_FALSE(platf::touch_input::cursor_hidden());
}

TEST(MacosTouchInputTest, PenShowsThePointer) {
  platf::touch_input::touch(display_port(), touch_event(LI_TOUCH_EVENT_DOWN, 0.1f, 0.1f));
  platf::touch_input::touch(display_port(), touch_event(LI_TOUCH_EVENT_CANCEL_ALL, 0.1f, 0.1f));
  ASSERT_TRUE(platf::touch_input::cursor_hidden());

  for (const auto type : {LI_TOUCH_EVENT_HOVER, LI_TOUCH_EVENT_DOWN, LI_TOUCH_EVENT_MOVE, LI_TOUCH_EVENT_UP, LI_TOUCH_EVENT_BUTTON_ONLY}) {
    platf::pen_update(nullptr, display_port(), pen_event(type, LI_PEN_BUTTON_SECONDARY));
  }
  EXPECT_FALSE(platf::touch_input::cursor_hidden());
}

TEST(MacosTouchInputTest, TouchIsAdvertisedWhenPenAndTouchAreEnabled) {
  const bool saved {config::input.native_pen_touch};
  config::input.native_pen_touch = true;
  EXPECT_NE(platf::get_capabilities() & platf::platform_caps::pen_touch, 0U);
  config::input.native_pen_touch = false;
  EXPECT_EQ(platf::get_capabilities() & platf::platform_caps::pen_touch, 0U);
  config::input.native_pen_touch = saved;
}

#endif
