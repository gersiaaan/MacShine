/**
 * @file src/platform/macos/touch_input.cpp
 * @brief Definitions for handling touchscreen and pen input from clients on macOS.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

// platform includes
#include <ApplicationServices/ApplicationServices.h>
#include <dispatch/dispatch.h>

// local includes
#include "src/logging.h"
#include "src/platform/macos/touch_input.h"

namespace platf::touch_input {

  namespace {
    using touch_gestures::action_kind_e;
    using touch_gestures::action_t;
    using touch_gestures::button_e;
    using touch_gestures::scroll_phase_e;

    constexpr std::chrono::milliseconds GLIDE_TICK {8};  ///< Interval of glide steps (about 120 Hz).

    std::mutex g_mutex;  ///< Guards the recognizer and the glide timer.
    touch_gestures::recognizer_t g_recognizer;  ///< Gestures of the client's touches.
    dispatch_source_t g_timer {nullptr};  ///< Glide timer; runs only while the content glides.

    // Used only on the posting queue.
    CGEventSourceRef g_source {nullptr};  ///< Source of the posted events.
    bool g_left_down {false};  ///< Whether a gesture or the pen holds the left button.
    AXUIElementRef g_dock_item {nullptr};  ///< Dock item under the finger when it touched down.
    std::atomic<double> g_display_middle_y {0.0};  ///< Vertical middle of the streamed display.
    bool g_right_down {false};  ///< Whether the pen holds the right button.
    double g_scroll_rest_x {0.0};  ///< Horizontal scroll fraction not posted yet.
    double g_scroll_rest_y {0.0};  ///< Vertical scroll fraction not posted yet.
    std::atomic<bool> g_cursor_hidden {false};  ///< Whether the pointer is hidden from the stream.
    std::atomic<std::int64_t> g_last_touch_ms {0};  ///< Time of the last touch event, for the diagnostic log.
#ifdef SUNSHINE_TESTS
    std::atomic<bool> g_enabled {false};  ///< Test builds never move the pointer of the machine running them.
#else
    std::atomic<bool> g_enabled {true};  ///< Whether events are posted.
#endif

    /**
     * @brief Get the event source, creating it on first use.
     * @return Event source.
     */
    CGEventSourceRef source() {
      if (!g_source) {
        g_source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
      }
      return g_source;
    }

    /**
     * @brief Post a mouse event.
     * @param type Event type.
     * @param point Pointer position.
     * @param button Mouse button of the event.
     * @param clicks Click count, 0 for moves.
     */
    void post_mouse(CGEventType type, CGPoint point, CGMouseButton button, int clicks) {
      CGEventRef event {CGEventCreateMouseEvent(source(), type, point, button)};
      if (!event) {
        return;
      }
      if (clicks > 0) {
        CGEventSetIntegerValueField(event, kCGMouseEventClickState, clicks);
      }
      CGEventPost(kCGHIDEventTap, event);
      CFRelease(event);
    }

    /**
     * @brief Post a pointer move, as a drag while a button is held.
     * @param point Pointer position.
     */
    void post_move(CGPoint point) {
      if (g_left_down) {
        post_mouse(kCGEventLeftMouseDragged, point, kCGMouseButtonLeft, 0);
      } else if (g_right_down) {
        post_mouse(kCGEventRightMouseDragged, point, kCGMouseButtonRight, 0);
      } else {
        post_mouse(kCGEventMouseMoved, point, kCGMouseButtonLeft, 0);
      }
    }

    /**
     * @brief Post a button press or release.
     * @param button Button.
     * @param down Whether the button is pressed.
     * @param point Pointer position.
     * @param clicks Click count.
     */
    void post_button(button_e button, bool down, CGPoint point, int clicks) {
      if (button == button_e::left) {
        g_left_down = down;
        post_mouse(down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp, point, kCGMouseButtonLeft, clicks);
      } else {
        g_right_down = down;
        post_mouse(down ? kCGEventRightMouseDown : kCGEventRightMouseUp, point, kCGMouseButtonRight, clicks);
      }
    }

    /**
     * @brief Take the whole points of a scroll distance, keeping the fraction for the next step.
     * @param distance Distance of this step.
     * @param rest Fraction left from earlier steps.
     * @return Whole points to post.
     */
    int whole_points(double distance, double &rest) {
      rest += distance;
      const auto whole {static_cast<int>(std::trunc(rest))};
      rest -= whole;
      return whole;
    }

    /**
     * @brief Post a scroll step as a trackpad gesture or glide event.
     * @param action Scroll action.
     */
    void post_scroll(const action_t &action) {
      if (action.phase == scroll_phase_e::began) {
        g_scroll_rest_x = 0.0;
        g_scroll_rest_y = 0.0;
      }
      const int dx {whole_points(action.dx, g_scroll_rest_x)};
      const int dy {whole_points(action.dy, g_scroll_rest_y)};

      std::int64_t scroll_phase {0};
      std::int64_t momentum_phase {kCGMomentumScrollPhaseNone};
      switch (action.phase) {
        case scroll_phase_e::began:
          scroll_phase = 1;
          break;
        case scroll_phase_e::changed:
          scroll_phase = 2;
          break;
        case scroll_phase_e::ended:
          scroll_phase = 4;
          break;
        case scroll_phase_e::momentum_began:
          momentum_phase = kCGMomentumScrollPhaseBegin;
          break;
        case scroll_phase_e::momentum_changed:
          momentum_phase = kCGMomentumScrollPhaseContinue;
          break;
        case scroll_phase_e::momentum_ended:
          momentum_phase = kCGMomentumScrollPhaseEnd;
          break;
      }
      // Steps of less than a point only carry the fraction forward.
      if (dx == 0 && dy == 0 && (action.phase == scroll_phase_e::changed || action.phase == scroll_phase_e::momentum_changed)) {
        return;
      }

      CGEventRef event {CGEventCreateScrollWheelEvent(source(), kCGScrollEventUnitPixel, 2, dy, dx)};
      if (!event) {
        return;
      }
      CGEventSetIntegerValueField(event, kCGScrollWheelEventIsContinuous, 1);
      CGEventSetIntegerValueField(event, kCGScrollWheelEventScrollPhase, scroll_phase);
      CGEventSetIntegerValueField(event, kCGScrollWheelEventMomentumPhase, momentum_phase);
      CGEventPost(kCGHIDEventTap, event);
      CFRelease(event);
    }

    /**
     * @brief Post the actions of a gesture; runs on the posting queue.
     * @param actions Actions in order.
     */
    void post_now(const std::vector<action_t> &actions) {
      for (const auto &action : actions) {
        const CGPoint point {action.point.x, action.point.y};
        switch (action.kind) {
          case action_kind_e::move:
            post_move(point);
            break;
          case action_kind_e::button:
            post_button(action.button, action.down, point, action.clicks);
            break;
          case action_kind_e::scroll:
            post_scroll(action);
            break;
        }
      }
    }

    /**
     * @brief Get the serial queue events are posted on, creating it on first use.
     *
     * Posting there keeps the order of all touch and pen events and the glide steps.
     *
     * @return Posting queue.
     */
    dispatch_queue_t queue() {
      static dispatch_queue_t posting {dispatch_queue_create("macshine.touch-input", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0))};
      return posting;
    }

    /**
     * @brief Run a function on the posting queue.
     * @param work Function to run.
     */
    void run_on_queue(std::function<void()> work) {
      dispatch_async_f(queue(), new std::function<void()>(std::move(work)), [](void *context) {
        const std::unique_ptr<std::function<void()>> queued {static_cast<std::function<void()> *>(context)};
        (*queued)();
      });
    }

    /**
     * @brief Move a tap's click onto a small button next to it; runs on the posting queue.
     * @param actions Actions of a gesture; only a left click is changed.
     */
    void snap_click(std::vector<action_t> &actions) {
      const auto press {std::ranges::find_if(actions, [](const action_t &action) {
        return action.kind == action_kind_e::button && action.down && action.button == button_e::left;
      })};
      if (press == actions.end()) {
        return;
      }
      const auto target {find_snap_target(press->point)};
      if (!target) {
        return;
      }
      for (auto &action : actions) {
        if (action.kind != action_kind_e::scroll) {
          action.point = *target;
        }
      }
    }

    /**
     * @brief Open the Dock item a tap started on instead of clicking; runs on the posting queue.
     *
     * The Dock magnifies its icons when the pointer comes in, which moves a neighbor under the
     * finger before the click. The item under the finger when it touched down is the one it meant.
     *
     * @param actions Actions of a gesture; when the item opened, the click is replaced by a move out of the Dock.
     * @return True when the item opened.
     */
    bool press_dock_item(std::vector<action_t> &actions) {
      const bool taps {std::ranges::any_of(actions, [](const action_t &action) {
        return action.kind == action_kind_e::button && action.down && action.button == button_e::left;
      })};
      if (!taps || !g_dock_item || AXUIElementPerformAction(g_dock_item, kAXPressAction) != kAXErrorSuccess) {
        return false;
      }
      std::erase_if(actions, [](const action_t &action) {
        return action.kind == action_kind_e::button;
      });
      // Move the hidden pointer out of the Dock, so it slides away once the app opened.
      const auto from {actions.empty() ? touch_gestures::point_t {} : actions.back().point};
      actions.push_back({.kind = action_kind_e::move, .point = {from.x, g_display_middle_y.load()}});
      return true;
    }

    /**
     * @brief Post the actions of a touch event in order, after the ones before.
     * @param actions Actions in order.
     * @param event_type Moonlight touch event type the actions came from.
     * @param point Position of the touch.
     */
    void post(std::vector<action_t> actions, std::uint8_t event_type, touch_gestures::point_t point) {
      if (!g_enabled.load()) {
        return;
      }
      run_on_queue([actions = std::move(actions), event_type, point]() mutable {
        if (event_type == LI_TOUCH_EVENT_DOWN) {
          // Before the pointer moves into the Dock and magnifies it.
          if (g_dock_item) {
            CFRelease(g_dock_item);
          }
          g_dock_item = dock_item_at(point);
        } else if (!press_dock_item(actions)) {
          snap_click(actions);
        }
        if (event_type != LI_TOUCH_EVENT_DOWN && event_type != LI_TOUCH_EVENT_MOVE && g_dock_item) {
          CFRelease(g_dock_item);
          g_dock_item = nullptr;
        }
        post_now(actions);
      });
    }

    /**
     * @brief Post the next glide step; runs on the posting queue.
     */
    void on_tick(void *) {
      std::lock_guard lock {g_mutex};
      if (const auto step {g_recognizer.tick(std::chrono::steady_clock::now())}) {
        post_now({*step});
      }
      if (!g_recognizer.gliding() && g_timer) {
        dispatch_source_cancel(g_timer);
        dispatch_release(g_timer);
        g_timer = nullptr;
      }
    }

    /**
     * @brief Start the glide timer if the content glides; called with g_mutex held.
     */
    void ensure_glide_timer() {
      if (g_timer || !g_recognizer.gliding()) {
        return;
      }
      g_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue());
      const auto interval {static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(GLIDE_TICK).count())};
      dispatch_source_set_timer(g_timer, dispatch_time(DISPATCH_TIME_NOW, static_cast<std::int64_t>(interval)), interval, interval / 10);
      dispatch_source_set_event_handler_f(g_timer, on_tick);
      dispatch_resume(g_timer);
    }
  }  // namespace

  bool in_title_bar(const CGRect &window, touch_gestures::point_t point) {
    return point.x >= CGRectGetMinX(window) && point.x < CGRectGetMaxX(window) &&
           point.y >= CGRectGetMinY(window) && point.y < CGRectGetMinY(window) + TITLE_BAR_HEIGHT;
  }

  bool role_blocks_window_drag(std::string_view role) {
    static constexpr std::string_view blocking[] {
      "AXButton",
      "AXCheckBox",
      "AXComboBox",
      "AXIncrementor",
      "AXLink",
      "AXList",
      "AXMenuButton",
      "AXOutline",
      "AXPopUpButton",
      "AXRadioButton",
      "AXScrollArea",
      "AXSlider",
      "AXTab",
      "AXTable",
      "AXTextArea",
      "AXTextField",
      "AXWebArea",
    };
    return std::ranges::find(blocking, role) != std::end(blocking);
  }

  namespace {
    /**
     * @brief Get a text attribute of an accessibility element, by default its role.
     * @param element Element.
     * @param attribute Attribute to read.
     * @return Text, or an empty string when it has none.
     */
    std::string ax_role(AXUIElementRef element, CFStringRef attribute = kAXRoleAttribute) {
      CFTypeRef value {};
      if (AXUIElementCopyAttributeValue(element, attribute, &value) != kAXErrorSuccess || !value) {
        return {};
      }
      std::string role;
      if (CFGetTypeID(value) == CFStringGetTypeID()) {
        char buffer[128] {};
        if (CFStringGetCString(static_cast<CFStringRef>(value), buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
          role = buffer;
        }
      }
      CFRelease(value);
      return role;
    }

    /**
     * @brief Check whether a control or content is under a point, up to its window.
     * @param point Desktop point.
     * @return True when one is, false when there is none or accessibility is not available.
     */
    bool control_at(touch_gestures::point_t point) {
      if (!AXIsProcessTrusted()) {
        return false;
      }
      AXUIElementRef system {AXUIElementCreateSystemWide()};
      AXUIElementRef element {};
      const AXError error {AXUIElementCopyElementAtPosition(system, static_cast<float>(point.x), static_cast<float>(point.y), &element)};
      CFRelease(system);
      if (error != kAXErrorSuccess || !element) {
        return false;
      }
      bool blocked {false};
      // The element itself or a few of its parents, such as the web area around a link's text.
      for (int depth = 0; element && depth < 5; ++depth) {
        const auto role {ax_role(element)};
        if (role == "AXWindow") {
          break;
        }
        if (role_blocks_window_drag(role)) {
          blocked = true;
          break;
        }
        CFTypeRef parent {};
        AXUIElementCopyAttributeValue(element, kAXParentAttribute, &parent);
        CFRelease(element);
        element = static_cast<AXUIElementRef>(parent);
      }
      if (element) {
        CFRelease(element);
      }
      return blocked;
    }

    /**
     * @brief Describe the accessibility element under a point, for the diagnostic log.
     * @param point Desktop point.
     * @return Role, subrole, title and description of the element.
     */
    std::string describe_element_at(touch_gestures::point_t point) {
      if (!AXIsProcessTrusted()) {
        return "(no accessibility access)";
      }
      AXUIElementRef system {AXUIElementCreateSystemWide()};
      AXUIElementRef element {};
      const AXError error {AXUIElementCopyElementAtPosition(system, static_cast<float>(point.x), static_cast<float>(point.y), &element)};
      CFRelease(system);
      if (error != kAXErrorSuccess || !element) {
        return "(nothing)";
      }
      std::string text {ax_role(element) + '/' + ax_role(element, kAXSubroleAttribute) + " \"" + ax_role(element, kAXTitleAttribute) + "\" \"" + ax_role(element, kAXDescriptionAttribute) + '"'};
      CFRelease(element);
      return text;
    }

    /**
     * @brief Get a number from a window info dictionary.
     * @param info Window info.
     * @param key Key.
     * @return The number, or 0 when missing.
     */
    double window_number(CFDictionaryRef info, CFStringRef key) {
      double value {};
      const auto number {static_cast<CFNumberRef>(CFDictionaryGetValue(info, key))};
      if (number) {
        CFNumberGetValue(number, kCFNumberDoubleType, &value);
      }
      return value;
    }
  }  // namespace

  std::optional<touch_gestures::point_t> choose_snap_target(touch_gestures::point_t tap, const std::vector<CGRect> &buttons) {
    // A button right under the finger, of any size, takes the tap as it is.
    if (std::ranges::any_of(buttons, [&](const CGRect &frame) {
          return CGRectGetWidth(frame) > 0 && CGRectGetHeight(frame) > 0 && CGRectContainsPoint(frame, CGPointMake(tap.x, tap.y));
        })) {
      return std::nullopt;
    }
    std::optional<touch_gestures::point_t> best;
    double best_distance {0};
    for (const auto &frame : buttons) {
      if (CGRectGetWidth(frame) <= 0 || CGRectGetHeight(frame) <= 0 || CGRectGetWidth(frame) > SNAP_BUTTON_SIZE || CGRectGetHeight(frame) > SNAP_BUTTON_SIZE) {
        continue;
      }
      if (!CGRectContainsPoint(CGRectInset(frame, -SNAP_RADIUS, -SNAP_RADIUS), CGPointMake(tap.x, tap.y))) {
        continue;
      }
      const touch_gestures::point_t center {CGRectGetMidX(frame), CGRectGetMidY(frame)};
      const double distance {std::hypot(center.x - tap.x, center.y - tap.y)};
      if (!best || distance < best_distance) {
        best = center;
        best_distance = distance;
      }
    }
    return best;
  }

  namespace {
    /**
     * @brief Read the frame of an accessibility element.
     * @param element Element.
     * @return Frame in desktop points, or nothing when it has none.
     */
    std::optional<CGRect> ax_frame(AXUIElementRef element) {
      CFTypeRef position_value {};
      CFTypeRef size_value {};
      std::optional<CGRect> frame;
      if (AXUIElementCopyAttributeValue(element, kAXPositionAttribute, &position_value) == kAXErrorSuccess && position_value &&
          AXUIElementCopyAttributeValue(element, kAXSizeAttribute, &size_value) == kAXErrorSuccess && size_value) {
        CGPoint position {};
        CGSize size {};
        if (AXValueGetValue(static_cast<AXValueRef>(position_value), kAXValueTypeCGPoint, &position) &&
            AXValueGetValue(static_cast<AXValueRef>(size_value), kAXValueTypeCGSize, &size)) {
          frame = CGRect {position, size};
        }
      }
      if (position_value) {
        CFRelease(position_value);
      }
      if (size_value) {
        CFRelease(size_value);
      }
      return frame;
    }

    /**
     * @brief Collect the frames of buttons in a band, searching an element's descendants.
     * @param element Element to search; not released.
     * @param band Area to search.
     * @param depth Remaining depth.
     * @param budget Remaining number of elements to visit.
     * @param buttons Receives the button frames.
     * @param log Receives a description of the visited buttons, for the diagnostic log.
     */
    void collect_buttons(AXUIElementRef element, const CGRect &band, int depth, int &budget, std::vector<CGRect> &buttons, std::string &log) {
      CFTypeRef children {};
      if (depth <= 0 || AXUIElementCopyAttributeValue(element, kAXChildrenAttribute, &children) != kAXErrorSuccess || !children) {
        return;
      }
      if (CFGetTypeID(children) == CFArrayGetTypeID()) {
        const auto array {static_cast<CFArrayRef>(children)};
        for (CFIndex index = 0; index < CFArrayGetCount(array) && budget > 0; ++index) {
          --budget;
          const auto child {static_cast<AXUIElementRef>(const_cast<void *>(CFArrayGetValueAtIndex(array, index)))};
          const auto role {ax_role(child)};
          if (role == "AXWebArea") {
            continue;
          }
          const auto frame {ax_frame(child)};
          // Elements outside the band are skipped, with everything in them.
          if (frame && !CGRectIntersectsRect(*frame, band)) {
            continue;
          }
          if (role == "AXButton" && frame) {
            buttons.push_back(*frame);
            log += ' ' + ax_role(child, kAXDescriptionAttribute) + '@' + std::to_string(static_cast<int>(frame->origin.x)) + ',' +
                   std::to_string(static_cast<int>(frame->origin.y)) + '/' + std::to_string(static_cast<int>(frame->size.width)) + 'x' +
                   std::to_string(static_cast<int>(frame->size.height));
          }
          collect_buttons(child, band, depth - 1, budget, buttons, log);
        }
      }
      CFRelease(children);
    }
  }  // namespace

  std::optional<touch_gestures::point_t> find_snap_target(touch_gestures::point_t tap) {
    if (!AXIsProcessTrusted()) {
      return std::nullopt;
    }
    AXUIElementRef system {AXUIElementCreateSystemWide()};
    AXUIElementRef hit {};
    const AXError error {AXUIElementCopyElementAtPosition(system, static_cast<float>(tap.x), static_cast<float>(tap.y), &hit)};
    CFRelease(system);
    if (error != kAXErrorSuccess || !hit) {
      return std::nullopt;
    }
    AXUIElementRef window {};
    if (ax_role(hit) == "AXWindow") {
      window = static_cast<AXUIElementRef>(CFRetain(hit));
    } else {
      CFTypeRef value {};
      if (AXUIElementCopyAttributeValue(hit, kAXWindowAttribute, &value) == kAXErrorSuccess && value) {
        window = static_cast<AXUIElementRef>(value);
      }
    }
    CFRelease(hit);
    if (!window) {
      return std::nullopt;
    }

    std::optional<touch_gestures::point_t> target;
    const auto window_frame {ax_frame(window)};
    if (window_frame && in_title_bar(*window_frame, tap)) {
      const CGRect band {window_frame->origin, {window_frame->size.width, TITLE_BAR_HEIGHT}};
      std::vector<CGRect> buttons;
      std::string log;
      int budget {600};
      collect_buttons(window, band, 10, budget, buttons, log);
      target = choose_snap_target(tap, buttons);
      // MacShine: temporary diagnostics.
      BOOST_LOG(info) << "[touch] title bar buttons:" << (log.empty() ? " none" : log) << " -> "
                      << (target ? std::to_string(static_cast<int>(target->x)) + ',' + std::to_string(static_cast<int>(target->y)) : std::string {"no snap"});
    }
    CFRelease(window);
    return target;
  }

  AXUIElementRef dock_item_at(touch_gestures::point_t point) {
    if (!AXIsProcessTrusted()) {
      return nullptr;
    }
    AXUIElementRef system {AXUIElementCreateSystemWide()};
    AXUIElementRef element {};
    const AXError error {AXUIElementCopyElementAtPosition(system, static_cast<float>(point.x), static_cast<float>(point.y), &element)};
    CFRelease(system);
    if (error != kAXErrorSuccess || !element) {
      return nullptr;
    }
    if (ax_role(element) != "AXDockItem") {
      CFRelease(element);
      return nullptr;
    }
    return element;
  }

  bool starts_window_drag(touch_gestures::point_t point) {
    CFArrayRef windows {CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID)};
    if (!windows) {
      return false;
    }
    bool in_band {false};
    // Front to back: the first window under the finger is the one it touches.
    for (CFIndex index = 0; index < CFArrayGetCount(windows); ++index) {
      const auto info {static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, index))};
      CGRect bounds {};
      const auto bounds_info {static_cast<CFDictionaryRef>(CFDictionaryGetValue(info, kCGWindowBounds))};
      if (!bounds_info || !CGRectMakeWithDictionaryRepresentation(bounds_info, &bounds) || !CGRectContainsPoint(bounds, CGPointMake(point.x, point.y))) {
        continue;
      }
      if (window_number(info, kCGWindowAlpha) <= 0) {
        continue;
      }
      // Menus, the menu bar and the Dock are on other layers and are never dragged.
      in_band = window_number(info, kCGWindowLayer) == 0 && in_title_bar(bounds, point);
      break;
    }
    CFRelease(windows);
    return in_band && !control_at(point);
  }

  touch_gestures::point_t to_display(const touch_port_t &touch_port, float x, float y) {
    // Positions arrive as fractions of the streamed display (see monitor_touch_port in input.cpp).
    return {
      touch_port.offset_x + std::clamp(static_cast<double>(x), 0.0, 1.0) * touch_port.width,
      touch_port.offset_y + std::clamp(static_cast<double>(y), 0.0, 1.0) * touch_port.height,
    };
  }

  void touch(const touch_port_t &touch_port, const touch_input_t &touch) {
    const auto point {to_display(touch_port, touch.x, touch.y)};
    const auto now {std::chrono::steady_clock::now()};

    std::lock_guard lock {g_mutex};
    static const bool drag_area_set {[] {
      g_recognizer.set_drag_area(starts_window_drag);
      return true;
    }()};
    (void) drag_area_set;
    g_recognizer.set_bottom_edge(touch_port.offset_y + touch_port.height);
    g_display_middle_y.store(touch_port.offset_y + touch_port.height / 2.0);
    std::vector<action_t> actions;
    switch (touch.eventType) {
      case LI_TOUCH_EVENT_DOWN:
        g_cursor_hidden.store(true);
        actions = g_recognizer.down(touch.pointerId, point, now);
        break;
      case LI_TOUCH_EVENT_MOVE:
        actions = g_recognizer.move(touch.pointerId, point, now);
        break;
      case LI_TOUCH_EVENT_UP:
        actions = g_recognizer.up(touch.pointerId, point, now);
        break;
      case LI_TOUCH_EVENT_CANCEL:
      case LI_TOUCH_EVENT_CANCEL_ALL:
        actions = g_recognizer.cancel();
        break;
      default:
        // Hovering fingers do nothing.
        return;
    }
    // MacShine: temporary diagnostics for taps that do not register at once.
    g_last_touch_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    {
      std::string summary;
      for (const auto &action : actions) {
        summary += action.kind == action_kind_e::move ? " move" : action.kind == action_kind_e::scroll ? " scroll" : (action.down ? " press" : " release");
        if (action.kind == action_kind_e::button) {
          summary += action.button == button_e::left ? "L" : "R";
          summary += std::to_string(action.clicks);
        }
      }
      const bool clicks {std::ranges::any_of(actions, [](const action_t &action) {
        return action.kind == action_kind_e::button && action.down;
      })};
      BOOST_LOG(info) << "[touch] event " << static_cast<int>(touch.eventType) << " id " << touch.pointerId << " at "
                      << static_cast<int>(point.x) << ',' << static_cast<int>(point.y) << " ->" << (summary.empty() ? " nothing" : summary)
                      << (clicks ? " on " + describe_element_at(point) : std::string {});
    }
    post(std::move(actions), touch.eventType, point);
    if (g_enabled.load()) {
      ensure_glide_timer();
    }
  }

  void pen(const touch_port_t &touch_port, const pen_input_t &pen) {
    const auto point {to_display(touch_port, pen.x, pen.y)};
    const CGPoint location {point.x, point.y};
    g_cursor_hidden.store(false);
    if (!g_enabled.load()) {
      return;
    }

    const auto type {pen.eventType};
    const auto buttons {pen.penButtons};
    run_on_queue([type, buttons, location] {
      switch (type) {
        case LI_TOUCH_EVENT_HOVER:
        case LI_TOUCH_EVENT_MOVE:
          post_move(location);
          break;
        case LI_TOUCH_EVENT_DOWN:
          post_move(location);
          post_button((buttons & LI_PEN_BUTTON_SECONDARY) ? button_e::right : button_e::left, true, location, 1);
          break;
        case LI_TOUCH_EVENT_UP:
        case LI_TOUCH_EVENT_CANCEL:
        case LI_TOUCH_EVENT_CANCEL_ALL:
        case LI_TOUCH_EVENT_HOVER_LEAVE:
          if (g_left_down) {
            post_button(button_e::left, false, location, 1);
          }
          if (g_right_down) {
            post_button(button_e::right, false, location, 1);
          }
          break;
        default:
          break;
      }
    });
  }

  void mouse_used(std::string_view kind) {
    g_cursor_hidden.store(false);
    // MacShine: temporary diagnostics; logs mouse input that arrives together with touches.
    const auto now_ms {std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()};
    if (now_ms - g_last_touch_ms.load() < 3000) {
      BOOST_LOG(info) << "[touch] mouse input from the client: " << kind;
    }
  }

  void forget_touches() {
    g_last_touch_ms.store(0);
  }

  bool absolute_mouse_allowed() {
    const auto now_ms {std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()};
    const auto last_touch {g_last_touch_ms.load()};
    const bool allowed {last_touch == 0 || now_ms - last_touch >= TOUCH_MOUSE_QUIET.count()};
    if (!allowed) {
      // MacShine: temporary diagnostics.
      BOOST_LOG(info) << "[touch] dropped an absolute mouse move from the client";
    }
    return allowed;
  }

  bool cursor_hidden() {
    return g_cursor_hidden.load();
  }

  bool enabled() {
    return g_enabled.load();
  }

  void set_enabled(bool on) {
    g_enabled.store(on);
  }

}  // namespace platf::touch_input

bool macshine_stream_cursor_hidden(void) {
  return platf::touch_input::cursor_hidden();
}
