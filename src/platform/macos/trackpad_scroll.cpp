/**
 * @file src/platform/macos/trackpad_scroll.cpp
 * @brief Definitions for posting client scrolling as smooth trackpad scroll gestures on macOS.
 */
// standard includes
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>

// platform includes
#include <ApplicationServices/ApplicationServices.h>
#include <dispatch/dispatch.h>

// local includes
#include "src/platform/macos/trackpad_scroll.h"

namespace platf::trackpad_scroll {

  namespace {
    std::mutex g_mutex;  ///< Guards the state below.
    gesture_t g_gesture;  ///< Gesture of the current client scrolling.
    std::chrono::steady_clock::time_point g_last_packet {};  ///< When the last packet arrived.
    dispatch_queue_t g_queue {nullptr};  ///< Serial queue the tick runs on.
    dispatch_source_t g_timer {nullptr};  ///< Tick timer; runs only while a gesture is active.
    CGEventSourceRef g_source {nullptr};  ///< Event source for the posted events.
#ifdef SUNSHINE_TESTS
    std::atomic<bool> g_enabled {false};  ///< Test builds never scroll the machine running them.
#else
    std::atomic<bool> g_enabled {true};  ///< Whether scrolling is posted as trackpad gestures.
#endif

    /**
     * @brief Post one scroll event with its gesture phase.
     *
     * @param event Event to post.
     */
    void post_event(const event_t &event) {
      if (!g_source) {
        g_source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
      }
      CGEventRef cg_event {CGEventCreateScrollWheelEvent(g_source, kCGScrollEventUnitPixel, 2, event.vertical, event.horizontal)};
      if (!cg_event) {
        return;
      }
      CGEventSetIntegerValueField(cg_event, kCGScrollWheelEventIsContinuous, 1);
      CGEventSetIntegerValueField(cg_event, kCGScrollWheelEventScrollPhase, static_cast<int>(event.phase));
      CGEventSetIntegerValueField(cg_event, kCGScrollWheelEventMomentumPhase, kCGMomentumScrollPhaseNone);
      CGEventPost(kCGHIDEventTap, cg_event);
      CFRelease(cg_event);
    }

    /**
     * @brief Timer callback: post the next step, or end the gesture and stop the timer.
     */
    void on_tick(void *) {
      std::lock_guard lock {g_mutex};
      const bool idle {std::chrono::steady_clock::now() - g_last_packet >= gesture_t::END_AFTER};
      if (const auto event {g_gesture.tick(idle)}) {
        post_event(*event);
      }
      if (!g_gesture.active() && !g_gesture.pending() && g_timer) {
        dispatch_source_cancel(g_timer);
        dispatch_release(g_timer);
        g_timer = nullptr;
      }
    }

    /**
     * @brief Start the tick timer unless it is running. Requires g_mutex.
     */
    void ensure_timer() {
      if (g_timer) {
        return;
      }
      if (!g_queue) {
        g_queue = dispatch_queue_create("macshine.trackpad-scroll", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
      }
      g_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, g_queue);
      const auto interval {static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(gesture_t::TICK).count())};
      // The first step goes out right away; the rest follow on the tick.
      dispatch_source_set_timer(g_timer, dispatch_time(DISPATCH_TIME_NOW, 0), interval, interval / 10);
      dispatch_source_set_event_handler_f(g_timer, on_tick);
      dispatch_resume(g_timer);
    }

    /**
     * @brief Take the whole pixels of one tick's share and leave the rest collected.
     *
     * @param collected Collected distance in pixels; reduced by the returned amount.
     * @return Whole pixels to post: the tick's share, at least one pixel while one is left.
     */
    int take_step(double &collected) {
      int step {static_cast<int>(std::trunc(collected * gesture_t::SHARE_PER_TICK))};
      if (step == 0 && std::abs(collected) >= 1.0) {
        step = collected > 0 ? 1 : -1;
      }
      collected -= step;
      return step;
    }
  }  // namespace

  void gesture_t::add(int vertical, int horizontal) {
    const double scale {PIXELS_PER_DETENT / WHEEL_DELTA};
    vertical_ += vertical * scale;
    horizontal_ += horizontal * scale;
  }

  std::optional<event_t> gesture_t::tick(bool idle) {
    const int vertical {take_step(vertical_)};
    const int horizontal {take_step(horizontal_)};
    if (vertical != 0 || horizontal != 0) {
      const phase_e phase {active_ ? phase_e::changed : phase_e::began};
      active_ = true;
      return event_t {vertical, horizontal, phase};
    }
    if (idle && active_) {
      active_ = false;
      vertical_ = 0.0;
      horizontal_ = 0.0;
      return event_t {0, 0, phase_e::ended};
    }
    return std::nullopt;
  }

  bool gesture_t::active() const {
    return active_;
  }

  bool gesture_t::pending() const {
    return std::abs(vertical_) >= 1.0 || std::abs(horizontal_) >= 1.0;
  }

  bool post(int vertical, int horizontal) {
    std::lock_guard lock {g_mutex};
    g_last_packet = std::chrono::steady_clock::now();
    g_gesture.add(vertical, horizontal);
    ensure_timer();
    return true;
  }

  bool enabled() {
    return g_enabled.load();
  }

  void set_enabled(bool on) {
    g_enabled.store(on);
  }

}  // namespace platf::trackpad_scroll
