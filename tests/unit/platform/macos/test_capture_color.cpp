/**
 * @file tests/unit/platform/macos/test_capture_color.cpp
 * @brief Tests for matching macOS capture color formats to the stream colorspace.
 */

#ifdef __APPLE__
  // test includes
  #include "../../../tests_common.h"

  // platform includes
  #include <CoreGraphics/CoreGraphics.h>

  // local includes
  #include "src/platform/macos/capture_color.h"
  #include "src/platform/macos/nv12_zero_device.h"

using namespace platf::capture_color;

TEST(MacosCaptureColorTest, PixelFormatFollowsBitDepthAndRange) {
  EXPECT_EQ(pixel_format(false, false), kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
  EXPECT_EQ(pixel_format(false, true), kCVPixelFormatType_420YpCbCr8BiPlanarFullRange);
  EXPECT_EQ(pixel_format(true, false), kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange);
  EXPECT_EQ(pixel_format(true, true), kCVPixelFormatType_420YpCbCr10BiPlanarFullRange);
}

TEST(MacosCaptureColorTest, FullRangeDetectionMatchesPixelFormats) {
  EXPECT_TRUE(is_full_range(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange));
  EXPECT_TRUE(is_full_range(kCVPixelFormatType_420YpCbCr10BiPlanarFullRange));
  EXPECT_FALSE(is_full_range(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange));
  EXPECT_FALSE(is_full_range(kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange));
  EXPECT_FALSE(is_full_range(kCVPixelFormatType_32BGRA));
}

TEST(MacosCaptureColorTest, MatrixMatchesStreamColorspace) {
  EXPECT_EQ(ycbcr_matrix(video::colorspace_e::rec601), kCVImageBufferYCbCrMatrix_ITU_R_601_4);
  EXPECT_EQ(ycbcr_matrix(video::colorspace_e::rec709), kCVImageBufferYCbCrMatrix_ITU_R_709_2);
  EXPECT_EQ(ycbcr_matrix(video::colorspace_e::bt2020sdr), kCVImageBufferYCbCrMatrix_ITU_R_2020);
  EXPECT_EQ(ycbcr_matrix(video::colorspace_e::bt2020), kCVImageBufferYCbCrMatrix_ITU_R_2020);
  EXPECT_EQ(ycbcr_matrix(static_cast<video::colorspace_e>(99)), kCVImageBufferYCbCrMatrix_ITU_R_709_2);
}

TEST(MacosCaptureColorTest, SdrStreamsAreCapturedInSrgb) {
  EXPECT_EQ(rgb_color_space(video::colorspace_e::rec601), kCGColorSpaceSRGB);
  EXPECT_EQ(rgb_color_space(video::colorspace_e::rec709), kCGColorSpaceSRGB);
  EXPECT_EQ(rgb_color_space(video::colorspace_e::bt2020sdr), kCGColorSpaceITUR_2020);
  EXPECT_EQ(rgb_color_space(video::colorspace_e::bt2020), kCGColorSpaceITUR_2020);
  EXPECT_EQ(rgb_color_space(static_cast<video::colorspace_e>(99)), kCGColorSpaceSRGB);
}

TEST(MacosCaptureColorTest, ZeroCopyDeviceRequestsTheStreamRange) {
  struct case_t {
    platf::pix_fmt_e pix_fmt;
    bool full_range;
    OSType expected;
  };

  const case_t cases[] {
    {platf::pix_fmt_e::nv12, false, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange},
    {platf::pix_fmt_e::nv12, true, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange},
    {platf::pix_fmt_e::p010, false, kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange},
    {platf::pix_fmt_e::p010, true, kCVPixelFormatType_420YpCbCr10BiPlanarFullRange},
  };

  for (const auto &test_case : cases) {
    platf::nv12_zero_device device;
    int requested {};
    const auto result = device.init(
      nullptr,
      test_case.pix_fmt,
      test_case.full_range,
      [](AVVideo *, int, int) {
      },
      [&requested](AVVideo *, int pixel_format) {
        requested = pixel_format;
      }
    );

    EXPECT_EQ(result, 0);
    EXPECT_TRUE(device.hardware);
    EXPECT_EQ(static_cast<OSType>(requested), test_case.expected);
  }
}

#endif
