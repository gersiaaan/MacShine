/**
 * @file src/platform/macos/capture_color.h
 * @brief Declarations for matching macOS capture color formats to the stream colorspace.
 */
#pragma once

// standard includes
#include <string_view>

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
   * crushed. ScreenCaptureKit offers 4:4:4 only as 10-bit full range, so YUV 4:4:4 streams are captured
   * in that format and converted with encode_pixel_format before encoding.
   *
   * @param ten_bit Whether the stream uses 10-bit samples.
   * @param full_range Whether the stream signals full-range video.
   * @param yuv444 Whether the stream uses YUV 4:4:4 chroma sampling.
   * @return CoreVideo pixel format for ScreenCaptureKit.
   */
  OSType pixel_format(bool ten_bit, bool full_range, bool yuv444 = false);

  /**
   * @brief Pick the format VideoToolbox encodes an 8-bit YUV 4:4:4 stream from.
   *
   * @param full_range Whether the stream signals full-range video.
   * @return Biplanar 8-bit 4:4:4 CoreVideo pixel format in the stream range.
   */
  OSType yuv444_encode_format(bool full_range);

  /**
   * @brief Check whether VideoToolbox can encode a codec in YUV 4:4:4 for a stream.
   *
   * Only HEVC is encoded in 4:4:4 (RExt), and only with 8-bit samples: FFmpeg selects the Main10
   * profile for 10-bit VideoToolbox input, which subsamples the chroma to 4:2:0.
   *
   * @param codec_name FFmpeg encoder name, such as "hevc_videotoolbox".
   * @param dynamic_range Whether the stream uses 10-bit samples.
   * @return True when the codec is encoded with full chroma resolution.
   */
  bool yuv444_supported(std::string_view codec_name, bool dynamic_range);

  /**
   * @brief Check whether a CoreVideo YCbCr pixel format stores full-range samples.
   *
   * @param pixel_format CoreVideo pixel format.
   * @return True for the full-range biplanar 4:2:0 and 4:4:4 formats.
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
