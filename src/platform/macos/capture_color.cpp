/**
 * @file src/platform/macos/capture_color.cpp
 * @brief Definitions for matching macOS capture color formats to the stream colorspace.
 */
// platform includes
#include <CoreGraphics/CoreGraphics.h>

// local includes
#include "src/platform/macos/capture_color.h"

namespace platf::capture_color {

  OSType pixel_format(bool ten_bit, bool full_range) {
    if (ten_bit) {
      return full_range ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
    }
    return full_range ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
  }

  bool is_full_range(OSType pixel_format) {
    return pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange || pixel_format == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
  }

  CFStringRef ycbcr_matrix(video::colorspace_e colorspace) {
    switch (colorspace) {
      case video::colorspace_e::rec601:
        return kCVImageBufferYCbCrMatrix_ITU_R_601_4;
      case video::colorspace_e::bt2020sdr:
      case video::colorspace_e::bt2020:
        return kCVImageBufferYCbCrMatrix_ITU_R_2020;
      case video::colorspace_e::rec709:
      default:
        return kCVImageBufferYCbCrMatrix_ITU_R_709_2;
    }
  }

  CFStringRef rgb_color_space(video::colorspace_e colorspace) {
    switch (colorspace) {
      case video::colorspace_e::bt2020sdr:
      case video::colorspace_e::bt2020:
        return kCGColorSpaceITUR_2020;
      case video::colorspace_e::rec601:
      case video::colorspace_e::rec709:
      default:
        return kCGColorSpaceSRGB;
    }
  }

}  // namespace platf::capture_color
