/**
 * @file src/platform/macos/trackpad_scroll.h
 * @brief Declarations for posting client scrolling as smooth trackpad scroll gestures on macOS.
 *
 * A Mac trackpad reports scrolling as many small, evenly spaced steps, grouped in phases: a gesture
 * begins, changes while the fingers move and ends when they lift. macOS and apps use these phases
 * for smooth scrolling and for the elastic bounce at the end of a page. Client scrolling arrives as
 * irregular wheel distances, sometimes several packets in the same millisecond. This module
 * collects the distance and hands it out in steps on a fixed tick, posted with gesture phases.
 */
#pragma once

// standard includes
#include <chrono>
#include <optional>

namespace platf::trackpad_scroll {

  /**
   * @brief macOS scroll phases (CGScrollPhase values).
   */
  enum class phase_e : int {
    began = 1,  ///< First event of a gesture.
    changed = 2,  ///< Further movement in the gesture.
    ended = 4,  ///< The gesture is over; carries no distance.
  };

  /**
   * @brief One scroll event to post.
   */
  struct event_t {
    int vertical;  ///< Vertical distance in pixels; positive scrolls up.
    int horizontal;  ///< Horizontal distance in pixels, with the client's sign.
    phase_e phase;  ///< Gesture phase of the event.
  };

  /**
   * @brief Collects wheel distances and hands them out as even steps of one gesture.
   */
  class gesture_t {
  public:
    static constexpr std::chrono::milliseconds TICK {8};  ///< Interval between posted steps (about 120 Hz).
    static constexpr double SHARE_PER_TICK {0.5};  ///< Share of the collected distance posted on each tick.
    static constexpr std::chrono::milliseconds END_AFTER {80};  ///< A pause this long ends the gesture.
    static constexpr double PIXELS_PER_DETENT {50.0};  ///< Pixels per wheel detent, as macOS scrolls by default.
    static constexpr int WHEEL_DELTA {120};  ///< High-resolution units per wheel detent.

    /**
     * @brief Add a scroll packet to the distance still to post.
     *
     * @param vertical High-resolution vertical distance (120 per detent); positive scrolls up.
     * @param horizontal High-resolution horizontal distance, with the client's sign.
     */
    void add(int vertical, int horizontal);

    /**
     * @brief Produce the event for one tick.
     *
     * Posts a share of the collected distance, at least one pixel while a pixel is left, so
     * bursts of packets turn into a smooth glide. Fractions of a pixel stay for the next tick.
     *
     * @param idle Whether no packet arrived for gesture_t::END_AFTER.
     * @return The event to post: a step, the end of the gesture once it is idle and nothing is
     * left, or nothing.
     */
    std::optional<event_t> tick(bool idle);

    /**
     * @brief Whether a gesture is in progress.
     * @return True between the first step and the end event.
     */
    bool active() const;

    /**
     * @brief Whether there is distance left to post.
     * @return True while at least one whole pixel is collected.
     */
    bool pending() const;

  private:
    bool active_ {false};  ///< Whether a began event was posted.
    double vertical_ {0.0};  ///< Vertical pixels not posted yet.
    double horizontal_ {0.0};  ///< Horizontal pixels not posted yet.
  };

  /**
   * @brief Post a client scroll packet as part of a smooth trackpad gesture.
   *
   * The distance is posted in steps on gesture_t::TICK; the gesture ends after
   * gesture_t::END_AFTER without further packets.
   *
   * @param vertical High-resolution vertical distance (120 per detent); positive scrolls up.
   * @param horizontal High-resolution horizontal distance, with the client's sign.
   * @return True when the packet was accepted.
   */
  bool post(int vertical, int horizontal);

  /**
   * @brief Whether client scrolling is posted as trackpad gestures.
   *
   * On by default. Tests that check scrolling through a simulated libvirtualhid mouse turn it off,
   * and test builds start with it off so they never scroll the machine running them.
   *
   * @return True when post() is used for scrolling.
   */
  bool enabled();

  /**
   * @brief Turn posting scrolling as trackpad gestures on or off.
   * @param on Whether to use post() for scrolling.
   */
  void set_enabled(bool on);

}  // namespace platf::trackpad_scroll
