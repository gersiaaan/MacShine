/**
 * @file src/platform/macos/frame_timing.h
 * @brief Declarations for converting macOS capture times to Sunshine frame timestamps.
 */
#pragma once

// platform includes
#include <CoreMedia/CoreMedia.h>

#ifdef __cplusplus
  // standard includes
  #include <chrono>
  #include <optional>

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

extern "C" {
#endif

/**
 * @brief Minimum time between captured frames for a stream frame rate, for the Objective-C capture.
 *
 * The interval is 90% of the frame time. The virtual display refreshes at twice the stream rate, so
 * a new frame comes exactly two refreshes after the last one. With the full frame time as the
 * minimum, ScreenCaptureKit rejects such a frame when it arrives a hair early and takes the next
 * refresh instead, which shows as uneven motion (frames 25 ms and then 8 ms apart at 60 fps).
 *
 * @param frame_rate Stream frame rate; 60 is used when it is not positive.
 * @return Minimum frame interval.
 */
CMTime macshine_capture_min_frame_interval(int frame_rate);

#ifdef __cplusplus
}
#endif
