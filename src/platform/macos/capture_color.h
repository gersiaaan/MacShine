/**
 * @file src/platform/macos/capture_color.h
 * @brief Declarations for matching macOS capture color formats to the stream colorspace.
 */
#pragma once

// platform includes
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>

// local includes
#include "src/video_colorspace.h"

namespace platf::capture_color {

  /**
   * @brief Pick the biplanar YCbCr capture format for a stream.
   *
   * The capture must already use the range the encoder signals to the client. Otherwise limited-range
   * samples are decoded as full range (or the other way around) and the picture looks washed out or
   * crushed.
   *
   * @param ten_bit Whether the stream uses 10-bit samples.
   * @param full_range Whether the stream signals full-range video.
   * @return CoreVideo pixel format for ScreenCaptureKit and VideoToolbox.
   */
  OSType pixel_format(bool ten_bit, bool full_range);

  /**
   * @brief Check whether a CoreVideo YCbCr pixel format stores full-range samples.
   *
   * @param pixel_format CoreVideo pixel format.
   * @return True for the full-range biplanar formats.
   */
  bool is_full_range(OSType pixel_format);

  /**
   * @brief Pick the RGB to YCbCr matrix that matches the stream colorspace.
   *
   * @param colorspace Stream colorspace requested by the client.
   * @return CoreVideo YCbCr matrix name.
   */
  CFStringRef ycbcr_matrix(video::colorspace_e colorspace);

  /**
   * @brief Pick the RGB color space the desktop is converted into before YCbCr conversion.
   *
   * SDR streams use sRGB, whose primaries equal Rec. 709. Wide-gamut desktop content, such as
   * Display P3, is then mapped into the gamut that SDR clients display instead of being shown
   * oversaturated.
   *
   * @param colorspace Stream colorspace requested by the client.
   * @return CoreGraphics color space name.
   */
  CFStringRef rgb_color_space(video::colorspace_e colorspace);

}  // namespace platf::capture_color
