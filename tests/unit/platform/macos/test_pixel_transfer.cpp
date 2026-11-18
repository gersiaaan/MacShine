/**
 * @file tests/unit/platform/macos/test_pixel_transfer.cpp
 * @brief Tests for converting captured frames into the format VideoToolbox encodes.
 */

#ifdef __APPLE__
  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <memory>

  // platform includes
  #include <CoreMedia/CoreMedia.h>

// lib includes
extern "C" {
  #include <libavutil/frame.h>
}

  // local includes
  #include "src/platform/macos/av_img_t.h"
  #include "src/platform/macos/nv12_zero_device.h"
  #include "src/platform/macos/pixel_transfer.h"

namespace {
  /**
   * @brief Create a frame like the 4:4:4 frames ScreenCaptureKit delivers.
   * @param width Frame width.
   * @param height Frame height.
   * @return Pixel buffer the caller releases.
   */
  CVPixelBufferRef make_capture_frame(size_t width, size_t height) {
    CFDictionaryRef io_surface = CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *keys[] {kCVPixelBufferIOSurfacePropertiesKey};
    const void *values[] {io_surface};
    CFDictionaryRef attributes = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CVPixelBufferRef frame = nullptr;
    CVPixelBufferCreate(nullptr, width, height, kCVPixelFormatType_444YpCbCr10BiPlanarFullRange, attributes, &frame);
    CFRelease(attributes);
    CFRelease(io_surface);
    if (frame) {
      CVBufferSetAttachment(frame, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    }
    return frame;
  }
}  // namespace

TEST(MacosPixelTransferTest, ConvertsTo8BitYuv444AndKeepsTheMatrix) {
  platf::pixel_transfer_t transfer {kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange};
  EXPECT_EQ(transfer.format(), kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange);

  CVPixelBufferRef source = make_capture_frame(320, 200);
  ASSERT_NE(source, nullptr);
  CVPixelBufferRef converted = transfer.convert(source);
  ASSERT_NE(converted, nullptr);

  EXPECT_EQ(CVPixelBufferGetPixelFormatType(converted), kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange);
  EXPECT_EQ(CVPixelBufferGetWidth(converted), 320u);
  EXPECT_EQ(CVPixelBufferGetHeight(converted), 200u);
  // Both planes have full resolution: the chroma is not subsampled.
  EXPECT_EQ(CVPixelBufferGetWidthOfPlane(converted, 1), 320u);
  EXPECT_EQ(CVPixelBufferGetHeightOfPlane(converted, 1), 200u);
  CFTypeRef matrix = CVBufferCopyAttachment(converted, kCVImageBufferYCbCrMatrixKey, nullptr);
  ASSERT_NE(matrix, nullptr);
  EXPECT_TRUE(CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2));
  CFRelease(matrix);

  CVPixelBufferRelease(converted);
  CVPixelBufferRelease(source);
}

TEST(MacosPixelTransferTest, FollowsSizeChanges) {
  platf::pixel_transfer_t transfer {kCVPixelFormatType_444YpCbCr8BiPlanarFullRange};
  for (const auto &[width, height] : {std::pair<size_t, size_t> {320, 200}, {320, 200}, {640, 400}}) {
    CVPixelBufferRef source = make_capture_frame(width, height);
    ASSERT_NE(source, nullptr);
    CVPixelBufferRef converted = transfer.convert(source);
    ASSERT_NE(converted, nullptr);
    EXPECT_EQ(CVPixelBufferGetWidth(converted), width);
    EXPECT_EQ(CVPixelBufferGetHeight(converted), height);
    CVPixelBufferRelease(converted);
    CVPixelBufferRelease(source);
  }
}

TEST(MacosPixelTransferTest, RejectsAMissingFrame) {
  platf::pixel_transfer_t transfer {kCVPixelFormatType_444YpCbCr8BiPlanarFullRange};
  EXPECT_EQ(transfer.convert(nullptr), nullptr);
}

TEST(MacosPixelTransferTest, RejectsAFormatItCannotProduce) {
  platf::pixel_transfer_t transfer {0};
  CVPixelBufferRef source = make_capture_frame(320, 200);
  ASSERT_NE(source, nullptr);
  EXPECT_EQ(transfer.convert(source), nullptr);
  CVPixelBufferRelease(source);
}

TEST(MacosPixelTransferTest, ZeroCopyDeviceEncodesTheConvertedYuv444Frame) {
  platf::nv12_zero_device device;
  ASSERT_EQ(device.init(nullptr, platf::pix_fmt_e::nv24, false, [](AVVideo *, int, int) {}, [](AVVideo *, int) {}), 0);
  ASSERT_EQ(device.set_frame(av_frame_alloc(), nullptr), 0);

  CVPixelBufferRef source = make_capture_frame(320, 200);
  ASSERT_NE(source, nullptr);
  CMVideoFormatDescriptionRef description = nullptr;
  CMSampleBufferRef sample = nullptr;
  CMSampleTimingInfo timing {kCMTimeInvalid, kCMTimeZero, kCMTimeInvalid};
  ASSERT_EQ(CMVideoFormatDescriptionCreateForImageBuffer(nullptr, source, &description), noErr);
  ASSERT_EQ(CMSampleBufferCreateReadyWithImageBuffer(nullptr, source, description, &timing, &sample), noErr);

  platf::av_img_t img;
  img.sample_buffer = std::make_shared<platf::av_sample_buf_t>(sample);
  img.pixel_buffer = std::make_shared<platf::av_pixel_buf_t>(img.sample_buffer->buf);

  ASSERT_EQ(device.convert(img), 0);
  auto *encoded = reinterpret_cast<CVPixelBufferRef>(device.frame->data[3]);
  ASSERT_NE(encoded, nullptr);
  EXPECT_NE(encoded, source);
  EXPECT_EQ(CVPixelBufferGetPixelFormatType(encoded), kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange);

  img.pixel_buffer.reset();
  img.sample_buffer.reset();
  CFRelease(sample);
  CFRelease(description);
  CVPixelBufferRelease(source);
}

#endif
