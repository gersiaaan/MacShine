/**
 * @file src/platform/macos/touch_input.h
 * @brief Declarations for handling touchscreen and pen input from clients on macOS.
 */
#pragma once

#ifdef __cplusplus
  // standard includes
  #include <chrono>
  #include <optional>
  #include <string_view>
  #include <vector>

  // platform includes
  #include <ApplicationServices/ApplicationServices.h>

  // local includes
  #include "src/platform/common.h"
  #include "src/platform/macos/touch_gestures.h"

/**
 * @brief Touchscreen and pen input on macOS, which has no touch input of its own.
 *
 * Touches are recognized as gestures (see touch_gestures::recognizer_t) and posted as mouse and
 * scroll events. The pointer is hidden from the stream while the screen is touched and comes back
 * with the next mouse input.
 */
namespace platf::touch_input {

  /**
   * @brief Handle a touch event from a client.
   * @param touch_port Geometry of the streamed display.
   * @param touch Touch event; x and y are fractions of the streamed display.
   */
  void touch(const touch_port_t &touch_port, const touch_input_t &touch);

  /**
   * @brief Handle a pen event from a client, as a mouse with the pen tip as the button.
   * @param touch_port Geometry of the streamed display.
   * @param pen Pen event; x and y are fractions of the streamed display.
   */
  void pen(const touch_port_t &touch_port, const pen_input_t &pen);

  /**
   * @brief Note mouse input from a client, which shows the pointer in the stream again.
   * @param kind Kind of mouse input, for the log.
   */
  void mouse_used(std::string_view kind = {});

  /**
   * @brief How long after a touch absolute mouse moves from the client are dropped.
   */
  constexpr std::chrono::milliseconds TOUCH_MOUSE_QUIET {1000};

  /**
   * @brief Check whether an absolute mouse move from the client is used.
   *
   * Clients also send some touches as absolute mouse moves. Those are dropped for a moment after a
   * touch, so they do not pull the pointer away from the finger.
   *
   * @return False within TOUCH_MOUSE_QUIET of a touch event.
   */
  bool absolute_mouse_allowed();

  /**
   * @brief Forget recent touches, so absolute mouse moves are used again right away.
   */
  void forget_touches();

  /**
   * @brief Check whether the pointer is hidden from the stream.
   * @return True after touch input until the next mouse input.
   */
  bool cursor_hidden();

  /**
   * @brief Map a touch position onto the desktop.
   * @param touch_port Position and size of the streamed display in desktop points.
   * @param x Horizontal position as a fraction of the display width.
   * @param y Vertical position as a fraction of the display height.
   * @return Desktop position, kept on the streamed display.
   */
  touch_gestures::point_t to_display(const touch_port_t &touch_port, float x, float y);

  /**
   * @brief Height of the band at the top of a window that drags it, in points.
   *
   * It covers the title bar and a unified toolbar, such as Safari's or Chrome's tab strip.
   */
  constexpr double TITLE_BAR_HEIGHT {52.0};

  /**
   * @brief Check whether a point lies in the title bar band of a window.
   * @param window Window bounds in desktop points.
   * @param point Point to check.
   * @return True within the top TITLE_BAR_HEIGHT points of the window.
   */
  bool in_title_bar(const CGRect &window, touch_gestures::point_t point);

  /**
   * @brief Check whether an accessibility role under the finger keeps a title bar drag from moving the window.
   *
   * Controls, text fields and content in the title bar band are used as they are, like the address
   * bar or a toolbar button.
   *
   * @param role Accessibility role, such as "AXButton".
   * @return True for controls and content.
   */
  bool role_blocks_window_drag(std::string_view role);

  /**
   * @brief Distance in points within which a tap in a title bar snaps to a small button.
   */
  constexpr double SNAP_RADIUS {12.0};

  /**
   * @brief Largest width or height in points of a button a tap snaps to.
   */
  constexpr double SNAP_BUTTON_SIZE {32.0};

  /**
   * @brief Pick the small button a tap snaps to.
   *
   * Buttons like a browser tab's close button are smaller than a fingertip, so a tap close to one
   * clicks its center.
   *
   * @param tap Tap position.
   * @param buttons Frames of the buttons near the tap.
   * @return Center of the nearest small button within SNAP_RADIUS, or nothing, also when the tap is
   *         right on a button of any size.
   */
  std::optional<touch_gestures::point_t> choose_snap_target(touch_gestures::point_t tap, const std::vector<CGRect> &buttons);

  /**
   * @brief Find the small button in a window's title bar band that a tap snaps to.
   * @param tap Tap position.
   * @return Center of the button, or nothing when there is none or accessibility is not available.
   */
  std::optional<touch_gestures::point_t> find_snap_target(touch_gestures::point_t tap);

  /**
   * @brief Get the Dock item under a point.
   * @param point Desktop point.
   * @return Dock item that the caller releases, or nullptr when there is none or accessibility is not available.
   */
  AXUIElementRef dock_item_at(touch_gestures::point_t point);

  /**
   * @brief Check whether a finger moving from a point should drag the window under it.
   *
   * The frontmost window at the point must be a normal window, the point must be in its title bar
   * band, and no control or content may be under it.
   *
   * @param point Desktop point.
   * @return True when the window should be dragged.
   */
  bool starts_window_drag(touch_gestures::point_t point);

  /**
   * @brief Check whether events are posted to the system.
   * @return False in test builds unless enabled.
   */
  bool enabled();

  /**
   * @brief Turn posting events to the system on or off.
   * @param on Whether to post events.
   */
  void set_enabled(bool on);

}  // namespace platf::touch_input

extern "C" {
#else
  #include <stdbool.h>
#endif

/**
 * @brief Check whether the pointer should be hidden from the stream, for the Objective-C capture.
 * @return True after touch input until the next mouse input.
 */
bool macshine_stream_cursor_hidden(void);

#ifdef __cplusplus
}
#endif
