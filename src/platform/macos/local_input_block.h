/**
 * @file src/platform/macos/local_input_block.h
 * @brief Ignore the Mac's own keyboard and trackpad while a stream is active (MacShine).
 */
#pragma once

// standard includes
#include <cstdint>

namespace platf::local_input_block {
  /**
   * @brief Decide whether an event reaching the host's event tap is dropped.
   *
   * Events posted by this process (the streaming client's input) always pass. Everything else, such as
   * the built-in keyboard and trackpad, is dropped, except the Command and Escape keys, so Cmd+Esc can
   * still end the stream.
   *
   * @param type CGEventType of the event.
   * @param keycode Virtual key code of a keyboard event, ignored for other events.
   * @param source_pid Process that posted the event, 0 for hardware.
   * @param own_pid Process id of Sunshine.
   * @return True when the event must not reach the system.
   */
  bool should_block(std::uint32_t type, std::int64_t keycode, std::int64_t source_pid, std::int64_t own_pid);

  /**
   * @brief Start dropping local input. Safe to call when already started.
   *
   * Installs an event tap on its own thread. Needs the Accessibility permission; without it a warning
   * is logged and local input keeps working.
   *
   * @return True when local input is being blocked.
   */
  bool start();

  /**
   * @brief Give local input back. Safe to call at any time.
   */
  void stop();

  /**
   * @brief Whether local input is currently being blocked.
   *
   * @return True between a successful start() and stop().
   */
  bool active();
}  // namespace platf::local_input_block
