/**
 * @file src/platform/macos/touch_gestures.cpp
 * @brief Definitions for turning touchscreen contacts into macOS pointer and scroll actions.
 */
// standard includes
#include <algorithm>
#include <cmath>
#include <utility>

// local includes
#include "src/platform/macos/touch_gestures.h"

namespace platf::touch_gestures {

  namespace {
    /**
     * @brief Distance between two points.
     * @param a First point.
     * @param b Second point.
     * @return Distance in points.
     */
    double distance(point_t a, point_t b) {
      return std::hypot(a.x - b.x, a.y - b.y);
    }

    /**
     * @brief Build a move action.
     * @param point Pointer position.
     * @return Action.
     */
    action_t move_action(point_t point) {
      return {.kind = action_kind_e::move, .point = point};
    }

    /**
     * @brief Build a button action.
     * @param button Button.
     * @param down Whether the button is pressed.
     * @param point Pointer position.
     * @param clicks Click count.
     * @return Action.
     */
    action_t button_action(button_e button, bool down, point_t point, int clicks) {
      return {.kind = action_kind_e::button, .point = point, .button = button, .down = down, .clicks = clicks};
    }

    /**
     * @brief Build a scroll action.
     * @param dx Horizontal distance.
     * @param dy Vertical distance.
     * @param phase Scroll phase.
     * @return Action.
     */
    action_t scroll_action(double dx, double dy, scroll_phase_e phase) {
      return {.kind = action_kind_e::scroll, .dx = dx, .dy = dy, .phase = phase};
    }

    /**
     * @brief Seconds between two times.
     * @param from Earlier time.
     * @param to Later time.
     * @return Seconds.
     */
    double seconds(recognizer_t::clock_t::time_point from, recognizer_t::clock_t::time_point to) {
      return std::chrono::duration<double>(to - from).count();
    }
  }  // namespace

  recognizer_t::contact_t *recognizer_t::find(std::uint32_t id) {
    const auto found {std::ranges::find(contacts_, id, &contact_t::id)};
    return found == contacts_.end() ? nullptr : &*found;
  }

  point_t recognizer_t::tracked_point() const {
    if (contacts_.size() >= 2) {
      return {(contacts_[0].last.x + contacts_[1].last.x) / 2, (contacts_[0].last.y + contacts_[1].last.y) / 2};
    }
    return contacts_.empty() ? point_t {} : contacts_.front().last;
  }

  void recognizer_t::add_sample(point_t point, clock_t::time_point now) {
    samples_.push_back({point, now});
    std::erase_if(samples_, [&](const sample_t &sample) {
      return now - sample.time > VELOCITY_WINDOW;
    });
  }

  void recognizer_t::end_scroll(std::vector<action_t> &actions, clock_t::time_point now) {
    actions.push_back(scroll_action(0, 0, scroll_phase_e::ended));

    // A finger that rested before lifting does not flick.
    std::erase_if(samples_, [&](const sample_t &sample) {
      return now - sample.time > VELOCITY_WINDOW;
    });
    if (samples_.size() < 2) {
      return;
    }
    const auto &first {samples_.front()};
    const auto &last {samples_.back()};
    const double elapsed {seconds(first.time, last.time)};
    if (elapsed <= 0) {
      return;
    }
    const double vx {(last.point.x - first.point.x) / elapsed};
    const double vy {(last.point.y - first.point.y) / elapsed};
    if (std::hypot(vx, vy) < MIN_FLICK_SPEED) {
      return;
    }
    glide_vx_ = vx;
    glide_vy_ = vy;
    glide_time_ = now;
    glide_started_ = false;
    gliding_ = true;
  }

  void recognizer_t::stop_glide(std::vector<action_t> &actions) {
    if (gliding_ && glide_started_) {
      actions.push_back(scroll_action(0, 0, scroll_phase_e::momentum_ended));
    }
    gliding_ = false;
    glide_started_ = false;
  }

  void recognizer_t::click(std::vector<action_t> &actions, button_e button, point_t point, clock_t::time_point now) {
    const bool repeats {
      click_count_ > 0 && button == last_click_button_ && now - last_click_time_ <= MULTI_CLICK_TIME &&
      distance(point, last_click_point_) <= MULTI_CLICK_DISTANCE
    };
    click_count_ = repeats ? std::min(click_count_ + 1, 3) : 1;
    last_click_time_ = now;
    last_click_point_ = point;
    last_click_button_ = button;

    actions.push_back(move_action(point));
    actions.push_back(button_action(button, true, point, click_count_));
    actions.push_back(button_action(button, false, point, click_count_));
  }

  void recognizer_t::reset() {
    mode_ = mode_e::idle;
    contacts_.clear();
    samples_.clear();
  }

  std::vector<action_t> recognizer_t::down(std::uint32_t id, point_t point, clock_t::time_point now) {
    std::vector<action_t> actions;
    if (find(id)) {
      return actions;
    }

    if (gliding_ && mode_ == mode_e::idle) {
      // The touch only catches the gliding content.
      stop_glide(actions);
      contacts_.push_back({id, point, point});
      mode_ = mode_e::stopped;
      return actions;
    }

    switch (mode_) {
      case mode_e::idle:
        contacts_.push_back({id, point, point});
        touched_ = now;
        samples_.clear();
        mode_ = mode_e::pending;
        actions.push_back(move_action(point));
        break;
      case mode_e::pending:
        contacts_.push_back({id, point, point});
        mode_ = mode_e::two_fingers;
        break;
      case mode_e::scrolling:
        contacts_.push_back({id, point, point});
        mode_ = mode_e::two_finger_scrolling;
        // Follow the middle of both fingers from here, without a jump.
        scrolled_to_ = tracked_point();
        samples_.clear();
        break;
      default:
        // Further fingers are not tracked.
        break;
    }
    return actions;
  }

  std::vector<action_t> recognizer_t::move(std::uint32_t id, point_t point, clock_t::time_point now) {
    std::vector<action_t> actions;
    auto *contact {find(id)};
    if (!contact) {
      return actions;
    }
    contact->last = point;

    switch (mode_) {
      case mode_e::pending:
        if (distance(point, contact->start) > SLOP) {
          const double rise {contact->start.y - point.y};
          if (bottom_edge_ && contact->start.y >= *bottom_edge_ - EDGE_ZONE && rise > std::abs(point.x - contact->start.x)) {
            // A hidden Dock shows while the pointer touches the bottom edge.
            mode_ = mode_e::stopped;
            actions.push_back(move_action({contact->start.x, *bottom_edge_ - 1}));
          } else if (now - touched_ >= LONG_PRESS || (drag_area_ && drag_area_(contact->start))) {
            mode_ = mode_e::dragging;
            actions.push_back(move_action(contact->start));
            actions.push_back(button_action(button_e::left, true, contact->start, 1));
            actions.push_back(move_action(point));
          } else {
            mode_ = mode_e::scrolling;
            // Scrolling goes to the content under the pointer, so put it under the finger.
            actions.push_back(move_action(contact->start));
            actions.push_back(scroll_action(point.x - contact->start.x, point.y - contact->start.y, scroll_phase_e::began));
            scrolled_to_ = point;
            add_sample(contact->start, touched_);
            add_sample(point, now);
          }
        }
        break;
      case mode_e::two_fingers:
        {
          const point_t start {(contacts_[0].start.x + contacts_[1].start.x) / 2, (contacts_[0].start.y + contacts_[1].start.y) / 2};
          const point_t tracked {tracked_point()};
          if (distance(tracked, start) > SLOP) {
            mode_ = mode_e::two_finger_scrolling;
            actions.push_back(move_action(start));
            actions.push_back(scroll_action(tracked.x - start.x, tracked.y - start.y, scroll_phase_e::began));
            scrolled_to_ = tracked;
            add_sample(tracked, now);
          }
          break;
        }
      case mode_e::scrolling:
      case mode_e::two_finger_scrolling:
        {
          const point_t tracked {tracked_point()};
          if (tracked.x != scrolled_to_.x || tracked.y != scrolled_to_.y) {
            actions.push_back(scroll_action(tracked.x - scrolled_to_.x, tracked.y - scrolled_to_.y, scroll_phase_e::changed));
            scrolled_to_ = tracked;
          }
          add_sample(tracked, now);
          break;
        }
      case mode_e::dragging:
        actions.push_back(move_action(point));
        break;
      default:
        break;
    }
    return actions;
  }

  std::vector<action_t> recognizer_t::up(std::uint32_t id, point_t point, clock_t::time_point now) {
    std::vector<action_t> actions;
    auto *contact {find(id)};
    if (!contact) {
      return actions;
    }
    contact->last = point;

    switch (mode_) {
      case mode_e::pending:
        click(actions, now - touched_ >= LONG_PRESS ? button_e::right : button_e::left, contact->start, now);
        reset();
        return actions;
      case mode_e::scrolling:
        end_scroll(actions, now);
        reset();
        return actions;
      case mode_e::dragging:
        actions.push_back(move_action(point));
        actions.push_back(button_action(button_e::left, false, point, 1));
        reset();
        return actions;
      case mode_e::two_fingers:
        click(actions, button_e::right, contacts_.front().start, now);
        mode_ = mode_e::stopped;
        break;
      case mode_e::two_finger_scrolling:
        end_scroll(actions, now);
        mode_ = mode_e::stopped;
        break;
      default:
        break;
    }

    std::erase_if(contacts_, [&](const contact_t &tracked) {
      return tracked.id == id;
    });
    if (contacts_.empty()) {
      reset();
    }
    return actions;
  }

  std::vector<action_t> recognizer_t::cancel() {
    std::vector<action_t> actions;
    stop_glide(actions);
    switch (mode_) {
      case mode_e::dragging:
        actions.push_back(button_action(button_e::left, false, contacts_.front().last, 1));
        break;
      case mode_e::scrolling:
      case mode_e::two_finger_scrolling:
        actions.push_back(scroll_action(0, 0, scroll_phase_e::ended));
        break;
      default:
        break;
    }
    reset();
    return actions;
  }

  std::optional<action_t> recognizer_t::tick(clock_t::time_point now) {
    if (!gliding_) {
      return std::nullopt;
    }
    const double elapsed {std::max(0.0, seconds(glide_time_, now))};
    glide_time_ = now;
    const double decay {std::exp(-elapsed / GLIDE_TIME_CONSTANT)};
    // Distance covered while the speed decays over the elapsed time.
    const double travelled {GLIDE_TIME_CONSTANT * (1 - decay)};
    const double dx {glide_vx_ * travelled};
    const double dy {glide_vy_ * travelled};
    glide_vx_ *= decay;
    glide_vy_ *= decay;

    if (std::hypot(glide_vx_, glide_vy_) < STOP_SPEED) {
      const bool started {glide_started_};
      gliding_ = false;
      glide_started_ = false;
      if (!started) {
        return std::nullopt;
      }
      return scroll_action(0, 0, scroll_phase_e::momentum_ended);
    }

    const auto phase {glide_started_ ? scroll_phase_e::momentum_changed : scroll_phase_e::momentum_began};
    glide_started_ = true;
    return scroll_action(dx, dy, phase);
  }

  bool recognizer_t::gliding() const {
    return gliding_;
  }

  void recognizer_t::set_bottom_edge(std::optional<double> bottom) {
    bottom_edge_ = bottom;
  }

  void recognizer_t::set_drag_area(drag_area_fn_t drag_area) {
    drag_area_ = std::move(drag_area);
  }

}  // namespace platf::touch_gestures
