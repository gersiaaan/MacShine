/**
 * @file src/platform/macos/virtual_display.h
 * @brief Declarations for per-session macOS virtual displays.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string>

namespace platf::virtual_display {
  /**
   * @brief Parameters for a virtual display that matches a Moonlight client.
   */
  struct request_t {
    int width;  ///< Client stream width in pixels.
    int height;  ///< Client stream height in pixels.
    int fps;  ///< Client stream refresh rate.
    bool hidpi;  ///< Create a Retina (HiDPI) display: width/2 x height/2 points backed by width x height pixels.
    bool make_primary;  ///< Make the virtual display the main display (menu bar and Dock) for the session.
    std::uint32_t serial;  ///< Stable per-client serial so macOS remembers each client's display separately.
    std::string name;  ///< Display name shown in System Settings.
  };

  /**
   * @brief Create the virtual display, replacing any display a previous session left behind.
   * @param request Display parameters.
   * @return The CGDirectDisplayID as a decimal string, or std::nullopt on failure.
   */
  std::optional<std::string> create(const request_t &request);

  /**
   * @brief Remove the virtual display if one exists. Safe to call at any time.
   */
  void destroy();

  /**
   * @brief Get the active virtual display.
   * @return The CGDirectDisplayID as a decimal string, or an empty string when none is active.
   */
  std::string active_display_id();

  /**
   * @brief Derive a stable non-zero serial number for a client.
   * @param client_key Any stable per-client value, such as the paired client certificate.
   * @return Serial number for the display descriptor.
   */
  std::uint32_t serial_for_client(const std::string &client_key);
}  // namespace platf::virtual_display
