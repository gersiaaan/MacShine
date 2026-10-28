/**
 * @file src/platform/macos/stop_hotkey.h
 * @brief Global host hotkey that ends every stream (MacShine).
 */
#pragma once

// standard includes
#include <functional>

namespace platf::stop_hotkey {
  /**
   * @brief Register Cmd+Esc as a global hotkey on the host.
   *
   * The hotkey works in every app and needs no extra permission. Keys injected by a streaming
   * client trigger it as well. The callback runs on a background thread, so it may block.
   * Requires a serviced main dispatch queue (the system tray's event loop).
   *
   * @param on_pressed Called each time the hotkey is pressed.
   * @return True when the hotkey was registered.
   */
  bool register_hotkey(std::function<void()> on_pressed);

  /**
   * @brief Remove the hotkey registered by register_hotkey(). Safe to call at any time.
   */
  void unregister_hotkey();
}  // namespace platf::stop_hotkey
