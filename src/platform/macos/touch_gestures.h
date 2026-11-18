/**
 * @file src/platform/macos/touch_gestures.h
 * @brief Declarations for turning touchscreen contacts into macOS pointer and scroll actions.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace platf::touch_gestures {

  /**
   * @brief Point on the desktop in CoreGraphics points.
   */
  struct point_t {
    double x;  ///< Horizontal position.
    double y;  ///< Vertical position, growing downward.
  };

  /**
   * @brief Mouse buttons a gesture can press.
   */
  enum class button_e {
    left,  ///< Primary button.
    right,  ///< Secondary button.
  };

  /**
   * @brief Phase of a scroll action, matching macOS gesture phases.
   */
  enum class scroll_phase_e {
    began,  ///< First scroll of a finger gesture.
    changed,  ///< Further scrolling while the finger moves.
    ended,  ///< The finger lifted; carries no distance.
    momentum_began,  ///< First step of the glide after a flick.
    momentum_changed,  ///< Further glide.
    momentum_ended,  ///< The glide stopped; carries no distance.
  };

  /**
   * @brief Kind of an action.
   */
  enum class action_kind_e {
    move,  ///< Move the pointer, dragging when a button is held.
    button,  ///< Press or release a button.
    scroll,  ///< Scroll the content under the pointer.
  };

  /**
   * @brief One pointer or scroll action for the host.
   */
  struct action_t {
    action_kind_e kind;  ///< Kind of action.
    point_t point {};  ///< Pointer position for move and button actions.
    button_e button {button_e::left};  ///< Button of a button action.
    bool down {};  ///< Whether a button action presses the button.
    int clicks {1};  ///< Click count of a button action, 2 for a double click.
    double dx {};  ///< Horizontal scroll distance in points; positive moves the content right.
    double dy {};  ///< Vertical scroll distance in points; positive moves the content down.
    scroll_phase_e phase {scroll_phase_e::began};  ///< Phase of a scroll action.
  };

  /**
   * @brief Recognizes touchscreen gestures and turns them into pointer and scroll actions.
   *
   * - The pointer moves under a finger as it touches down, so hover effects see it before a click.
   * - A tap clicks where the finger was; taps in quick succession double or triple click.
   * - A finger that moves scrolls the content with it, and a flick keeps it gliding.
   * - A finger that moves from a window's title bar drags the window.
   * - A finger that swipes up from the bottom edge moves the pointer to the edge, which shows a hidden Dock.
   * - Holding a finger still and lifting it right clicks.
   * - Holding a finger still and then moving it drags with the left button, which selects or moves things.
   * - Tapping with two fingers right clicks; moving two fingers scrolls.
   * - Touching the screen while the content glides stops it, without clicking.
   */
  class recognizer_t {
  public:
    using clock_t = std::chrono::steady_clock;  ///< Clock of the event times.

    static constexpr double SLOP {14.0};  ///< Points a finger may move and still tap.
    static constexpr std::chrono::milliseconds LONG_PRESS {450};  ///< Holding this long turns a touch into a right click or drag.
    static constexpr std::chrono::milliseconds MULTI_CLICK_TIME {400};  ///< Taps closer together than this add to the click count.
    static constexpr double MULTI_CLICK_DISTANCE {32.0};  ///< Taps closer together than this add to the click count.
    static constexpr std::chrono::milliseconds VELOCITY_WINDOW {80};  ///< Finger movement this recent sets the flick speed.
    static constexpr double MIN_FLICK_SPEED {200.0};  ///< Points per second a finger must move at to start a glide.
    static constexpr double STOP_SPEED {15.0};  ///< Points per second at which a glide stops.
    static constexpr double GLIDE_TIME_CONSTANT {0.4};  ///< Seconds in which the glide slows to about a third.
    static constexpr double EDGE_ZONE {60.0};  ///< Points above the bottom edge where a swipe up shows the Dock.

    /**
     * @brief Handle a finger touching the screen.
     * @param id Contact identifier.
     * @param point Contact position.
     * @param now Event time.
     * @return Actions to perform.
     */
    std::vector<action_t> down(std::uint32_t id, point_t point, clock_t::time_point now);

    /**
     * @brief Handle a finger moving on the screen.
     * @param id Contact identifier.
     * @param point Contact position.
     * @param now Event time.
     * @return Actions to perform.
     */
    std::vector<action_t> move(std::uint32_t id, point_t point, clock_t::time_point now);

    /**
     * @brief Handle a finger lifting off the screen.
     * @param id Contact identifier.
     * @param point Contact position.
     * @param now Event time.
     * @return Actions to perform.
     */
    std::vector<action_t> up(std::uint32_t id, point_t point, clock_t::time_point now);

    /**
     * @brief Handle a canceled contact, or all contacts canceled at once.
     *
     * A held button is released and scrolling ends without a glide.
     *
     * @return Actions to perform.
     */
    std::vector<action_t> cancel();

    /**
     * @brief Advance the glide after a flick.
     * @param now Current time.
     * @return The next glide step, or nothing when no glide is running.
     */
    std::optional<action_t> tick(clock_t::time_point now);

    /**
     * @brief Check whether the content is gliding.
     * @return True while tick produces steps.
     */
    bool gliding() const;

    /**
     * @brief Set the bottom edge of the display, where a swipe up shows the Dock.
     * @param bottom Bottom edge in desktop points, or nothing to turn the swipe off.
     */
    void set_bottom_edge(std::optional<double> bottom);

    /**
     * @brief Callback that checks whether a finger moving from a point drags instead of scrolling.
     */
    using drag_area_fn_t = std::function<bool(point_t)>;

    /**
     * @brief Set where a moving finger drags right away, such as window title bars.
     * @param drag_area Callback, or an empty function to always scroll.
     */
    void set_drag_area(drag_area_fn_t drag_area);

  private:
    /**
     * @brief What the current touch turned out to be.
     */
    enum class mode_e {
      idle,  ///< No finger on the screen.
      pending,  ///< One finger down, not moved yet.
      scrolling,  ///< One finger scrolls.
      dragging,  ///< One finger drags with the left button held.
      two_fingers,  ///< Two fingers down, not moved yet.
      two_finger_scrolling,  ///< Two fingers scroll.
      stopped,  ///< A finger stopped a glide and does nothing else.
    };

    /**
     * @brief A finger on the screen.
     */
    struct contact_t {
      std::uint32_t id;  ///< Contact identifier.
      point_t start;  ///< Where the finger touched down.
      point_t last;  ///< Latest position.
    };

    /**
     * @brief Position with its time, for the flick speed.
     */
    struct sample_t {
      point_t point;  ///< Tracked position.
      clock_t::time_point time;  ///< When it was there.
    };

    /**
     * @brief Find a tracked finger.
     * @param id Contact identifier.
     * @return The finger, or nullptr when it is not tracked.
     */
    contact_t *find(std::uint32_t id);

    /**
     * @brief Position the scroll follows: the finger, or the middle of two fingers.
     * @return Tracked position.
     */
    point_t tracked_point() const;

    /**
     * @brief Remember a tracked position for the flick speed.
     * @param point Tracked position.
     * @param now Event time.
     */
    void add_sample(point_t point, clock_t::time_point now);

    /**
     * @brief End scrolling and start a glide when the finger flicked.
     * @param actions Receives the ended scroll.
     * @param now Time the finger lifted.
     */
    void end_scroll(std::vector<action_t> &actions, clock_t::time_point now);

    /**
     * @brief Stop a running glide.
     * @param actions Receives the ended glide, if one was posted.
     */
    void stop_glide(std::vector<action_t> &actions);

    /**
     * @brief Click a button, counting quick repeated taps as double and triple clicks.
     * @param actions Receives the move and button actions.
     * @param button Button to click.
     * @param point Click position.
     * @param now Event time.
     */
    void click(std::vector<action_t> &actions, button_e button, point_t point, clock_t::time_point now);

    /**
     * @brief Forget all fingers.
     */
    void reset();

    mode_e mode_ {mode_e::idle};  ///< Current gesture.
    std::vector<contact_t> contacts_;  ///< Fingers on the screen, at most two tracked.
    clock_t::time_point touched_ {};  ///< When the first finger touched down.
    point_t scrolled_to_ {};  ///< Tracked position the scroll has followed so far.
    std::vector<sample_t> samples_;  ///< Recent tracked positions.
    double glide_vx_ {};  ///< Glide speed, points per second.
    double glide_vy_ {};  ///< Glide speed, points per second.
    bool glide_started_ {};  ///< Whether a momentum_began step went out.
    clock_t::time_point glide_time_ {};  ///< Time of the last glide step.
    bool gliding_ {};  ///< Whether a glide is running.
    clock_t::time_point last_click_time_ {};  ///< Time of the last tap.
    point_t last_click_point_ {};  ///< Position of the last tap.
    button_e last_click_button_ {button_e::left};  ///< Button of the last tap.
    int click_count_ {};  ///< Click count of the last tap.
    drag_area_fn_t drag_area_;  ///< Where a moving finger drags instead of scrolling.
    std::optional<double> bottom_edge_;  ///< Bottom edge of the display, if known.
  };

}  // namespace platf::touch_gestures
