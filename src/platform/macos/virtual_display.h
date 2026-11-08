/**
 * @file src/platform/macos/virtual_display.h
 * @brief Declarations for per-session macOS virtual displays.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace platf::virtual_display {
  constexpr int MIN_SCALE_PERCENT {50};  ///< Smallest desktop size relative to the client resolution (largest text).
  constexpr int MAX_SCALE_PERCENT {200};  ///< Largest desktop size relative to the client resolution (most space).
  /// Desktop sizes the virtual display offers in System Settings > Displays, in percent of the client resolution.
  constexpr int MAX_REFRESH_RATE {120};  ///< Highest refresh rate requested for the virtual display.
  constexpr std::array<int, 9> SIZE_PRESETS {75, 85, 90, 100, 115, 130, 150, 175, 200};

  /**
   * @brief Parameters for a virtual display that matches a Moonlight client.
   */
  struct request_t {
    int width;  ///< Client stream width in pixels.
    int height;  ///< Client stream height in pixels.
    int fps;  ///< Client stream refresh rate.
    bool hidpi;  ///< Create a Retina (HiDPI) display: width/2 x height/2 points backed by width x height pixels.
    bool make_primary;  ///< Make the virtual display the main display (menu bar and Dock) for the session.
    bool disable_others;  ///< Turn every other display off for the session, so only the virtual display remains.
    int scale_percent;  ///< Desktop size relative to the client resolution: above 100 gives more space, below 100 larger text.
    std::uint32_t serial;  ///< Stable per-client serial so macOS remembers each client's display separately.
    std::string name;  ///< Display name shown in System Settings.
  };

  /**
   * @brief One desktop size the virtual display offers.
   */
  struct size_option_t {
    int percent;  ///< Size relative to the client resolution.
    int width;  ///< Display width in pixels.
    int height;  ///< Display height in pixels.

    /**
     * @brief Compare two size options.
     * @param other Option to compare with.
     * @return True when both options are equal.
     */
    bool operator==(const size_option_t &other) const = default;
  };

  /**
   * @brief Create the virtual display, replacing any display a previous session left behind.
   * @param request Display parameters.
   * @return The CGDirectDisplayID as a decimal string, or std::nullopt on failure.
   */
  std::optional<std::string> create(const request_t &request);

  /**
   * @brief Wake the Mac's displays the way local input would, so a stream does not start black.
   * @return True when macOS accepted the user activity.
   */
  bool wake_for_stream();

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
   * @brief Turn back on host displays that a virtual display helper turned off and did not restore.
   *
   * Runs `sunshine_vd_helper --restore` and waits for it. Safe to call at any time; does nothing
   * when no display is recorded as turned off.
   */
  void restore_host_displays();

  /**
   * @brief Refresh rate of the virtual display for a client stream rate.
   *
   * The display runs at twice the stream rate, up to MAX_REFRESH_RATE and never below the stream
   * rate. A key press or mouse move is then composited sooner, so the next captured frame shows it
   * earlier, while the stream itself keeps the client's rate.
   *
   * @param client_fps Client stream refresh rate.
   * @return Refresh rate for the virtual display.
   */
  int display_refresh_rate(int client_fps);

  /**
   * @brief Pixel size of the virtual display for a request.
   *
   * The display is rendered at the client resolution times scale_percent and the capture scales it
   * back to the client resolution, so the stream resolution never changes. With HiDPI the size is
   * kept even, because the desktop is half of it in points.
   *
   * @param request Display parameters.
   * @return Width and height in pixels.
   */
  std::pair<int, int> display_pixel_size(const request_t &request);

  /**
   * @brief Desktop sizes the virtual display offers for a request.
   *
   * The configured size is always offered. The presets are added when they fit the helper's limits
   * and differ in pixel size, so the size can be changed in System Settings > Displays during a stream.
   *
   * @param request Display parameters.
   * @return Options sorted by percent.
   */
  std::vector<size_option_t> size_options(const request_t &request);

  /**
   * @brief Build the helper command line for a request.
   * @param helper Path of the helper executable, used as argv[0].
   * @param request Display parameters.
   * @return The helper's argv, without the terminating null pointer.
   */
  std::vector<std::string> helper_arguments(const std::string &helper, const request_t &request);

  /**
   * @brief Derive a stable non-zero serial number for a client.
   * @param client_key Any stable per-client value, such as the paired client certificate.
   * @return Serial number for the display descriptor.
   */
  std::uint32_t serial_for_client(const std::string &client_key);

  /**
   * @brief Build the line that tells the helper the virtual display's current size.
   *
   * @param pixel_width Display width in pixels.
   * @param pixel_height Display height in pixels.
   * @param points_width Display width in points.
   * @param points_height Display height in points.
   * @return A `SIZE` line for the helper's stdin, including the trailing newline.
   */
  std::string size_message(int pixel_width, int pixel_height, int points_width, int points_height);

  /**
   * @brief Tell the helper the size the virtual display has now.
   *
   * The helper owns the display and does not see sizes picked in System Settings, so Sunshine
   * reports them; the helper then remembers the size for the client. Does nothing when the display
   * is not the active virtual display.
   *
   * @param display_id Display the size belongs to.
   * @param pixel_width Display width in pixels.
   * @param pixel_height Display height in pixels.
   * @param points_width Display width in points.
   * @param points_height Display height in points.
   */
  void report_size(const std::string &display_id, int pixel_width, int pixel_height, int points_width, int points_height);
}  // namespace platf::virtual_display
