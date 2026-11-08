/**
 * @file src/platform/macos/frame_timing.h
 * @brief Declarations for converting macOS capture times to Sunshine frame timestamps.
 */
#pragma once

// standard includes
#include <chrono>
#include <optional>

// platform includes
#include <CoreMedia/CoreMedia.h>

namespace platf::frame_timing {

  /**
   * @brief Convert a host-clock time, such as a captured frame's presentation time, to steady_clock.
   *
   * ScreenCaptureKit and AVFoundation stamp frames with the host time clock. It pauses while the
   * Mac sleeps and steady_clock does not, so the conversion keeps the time's distance from now.
   *
   * @param host_time Time on the CoreMedia host time clock.
   * @return The same instant on steady_clock, or an empty value for an invalid time.
   */
  std::optional<std::chrono::steady_clock::time_point> host_time_to_steady(CMTime host_time);

  /**
   * @brief Pick the capture timestamp Sunshine reports for a frame.
   *
   * Moonlight shows the time from this timestamp to the encoded frame as host processing latency.
   * A frame without a usable time, or one re-delivered long after it was captured (an idle display
   * repeats its last frame), is stamped with the delivery time instead, so the statistic reflects
   * the pipeline rather than how long the screen was idle.
   *
   * @param presentation_time Presentation time of the captured frame on the host time clock.
   * @param now Delivery time of the frame.
   * @return Capture timestamp for the frame.
   */
  std::chrono::steady_clock::time_point capture_timestamp(CMTime presentation_time, std::chrono::steady_clock::time_point now);

}  // namespace platf::frame_timing
