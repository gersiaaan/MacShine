/**
 * @file tests/unit/platform/macos/test_frame_scaler.cpp
 * @brief Tests for downscaling captured frames on the GPU with a Lanczos filter.
 */

#ifdef __APPLE__
  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <cstring>
  #include <memory>

  // platform includes
  #include <CoreMedia/CoreMedia.h>

// lib includes
extern "C" {
  #include <libavutil/frame.h>
}

  // local includes
  #include "src/platform/macos/av_img_t.h"
  #include "src/platform/macos/frame_scaler.h"
  #include "src/platform/macos/nv12_zero_device.h"

namespace {
  /**
   * @brief Create an IOSurface-backed frame with every byte of each plane set to a value.
   * @param format Pixel format.
   * @param width Frame width.
   * @param height Frame height.
   * @param value Byte value of plane 0; other planes are set to 128.
   * @return Pixel buffer the caller releases.
   */
  CVPixelBufferRef make_frame(OSType format, size_t width, size_t height, int value) {
    CFDictionaryRef io_surface = CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *keys[] {kCVPixelBufferIOSurfacePropertiesKey, kCVPixelBufferMetalCompatibilityKey};
    const void *values[] {io_surface, kCFBooleanTrue};
    CFDictionaryRef attributes = CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CVPixelBufferRef frame = nullptr;
    CVPixelBufferCreate(nullptr, width, height, format, attributes, &frame);
    CFRelease(attributes);
    CFRelease(io_surface);
    if (!frame) {
      return nullptr;
    }
    CVBufferSetAttachment(frame, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    CVPixelBufferLockBaseAddress(frame, 0);
    if (CVPixelBufferIsPlanar(frame)) {
      for (size_t plane = 0; plane < CVPixelBufferGetPlaneCount(frame); ++plane) {
        std::memset(CVPixelBufferGetBaseAddressOfPlane(frame, plane), plane == 0 ? value : 128, CVPixelBufferGetBytesPerRowOfPlane(frame, plane) * CVPixelBufferGetHeightOfPlane(frame, plane));
      }
    } else {
      std::memset(CVPixelBufferGetBaseAddress(frame), value, CVPixelBufferGetBytesPerRow(frame) * CVPixelBufferGetHeight(frame));
    }
    CVPixelBufferUnlockBaseAddress(frame, 0);
    return frame;
  }

  /**
   * @brief Read a byte of a plane.
   * @param frame Pixel buffer.
   * @param plane Plane index.
   * @param row Row of the byte.
   * @return Value of the row's first sample, the high byte for 16-bit samples.
   */
  int byte_at(CVPixelBufferRef frame, size_t plane, size_t row) {
    CVPixelBufferLockBaseAddress(frame, kCVPixelBufferLock_ReadOnly);
    const bool planar {CVPixelBufferIsPlanar(frame) != 0};
    const auto *data {static_cast<const uint8_t *>(planar ? CVPixelBufferGetBaseAddressOfPlane(frame, plane) : CVPixelBufferGetBaseAddress(frame))};
    const auto stride {planar ? CVPixelBufferGetBytesPerRowOfPlane(frame, plane) : CVPixelBufferGetBytesPerRow(frame)};
    // 10-bit samples are stored in the high bits of 16-bit little-endian words.
    const bool sixteen_bit {planar && CVPixelBufferGetBytesPerRowOfPlane(frame, plane) >= CVPixelBufferGetWidthOfPlane(frame, plane) * 2};
    const int value {data[row * stride + (sixteen_bit ? 1 : 0)]};
    CVPixelBufferUnlockBaseAddress(frame, kCVPixelBufferLock_ReadOnly);
    return value;
  }
}  // namespace

TEST(MacosFrameScalerTest, DownscalesOnlyDisplaysLargerThanTheStream) {
  EXPECT_TRUE(platf::downscale_capture(2496, 1560, 1920, 1200));
  EXPECT_FALSE(platf::downscale_capture(1920, 1200, 1920, 1200));
  EXPECT_FALSE(platf::downscale_capture(1248, 780, 1920, 1200));
  EXPECT_FALSE(platf::downscale_capture(2496, 1000, 1920, 1200));
  EXPECT_FALSE(platf::downscale_capture(0, 0, 1920, 1200));
}

TEST(MacosFrameScalerTest, ScalesEveryCaptureFormatAndKeepsIt) {
  platf::frame_scaler_t scaler;
  if (!scaler.ready()) {
    GTEST_SKIP() << "Metal is not available";
  }
  for (const OSType format : {
         kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
         kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
         kCVPixelFormatType_420YpCbCr10BiPlanarFullRange,
         kCVPixelFormatType_444YpCbCr10BiPlanarFullRange,
         kCVPixelFormatType_32BGRA,
       }) {
    CVPixelBufferRef source = make_frame(format, 416, 260, 200);
    ASSERT_NE(source, nullptr);
    CVPixelBufferRef scaled = scaler.scale(source, 320, 200);
    ASSERT_NE(scaled, nullptr) << "format " << format;
    EXPECT_EQ(CVPixelBufferGetPixelFormatType(scaled), format);
    EXPECT_EQ(CVPixelBufferGetWidth(scaled), 320u);
    EXPECT_EQ(CVPixelBufferGetHeight(scaled), 200u);
    // A flat frame stays flat, also at its border.
    EXPECT_NEAR(byte_at(scaled, 0, 0), 200, 1) << "format " << format;
    EXPECT_NEAR(byte_at(scaled, 0, 100), 200, 1) << "format " << format;
    CFTypeRef matrix = CVBufferCopyAttachment(scaled, kCVImageBufferYCbCrMatrixKey, nullptr);
    ASSERT_NE(matrix, nullptr);
    EXPECT_TRUE(CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2));
    CFRelease(matrix);
    CVPixelBufferRelease(scaled);
    CVPixelBufferRelease(source);
  }
}

TEST(MacosFrameScalerTest, RejectsFramesItCannotScale) {
  platf::frame_scaler_t scaler;
  if (!scaler.ready()) {
    GTEST_SKIP() << "Metal is not available";
  }
  EXPECT_EQ(scaler.scale(nullptr, 320, 200), nullptr);

  CVPixelBufferRef source = make_frame(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, 416, 260, 0);
  ASSERT_NE(source, nullptr);
  EXPECT_EQ(scaler.scale(source, 0, 200), nullptr);
  CVPixelBufferRelease(source);

  CVPixelBufferRef unsupported = make_frame(kCVPixelFormatType_422YpCbCr8, 416, 260, 0);
  ASSERT_NE(unsupported, nullptr);
  EXPECT_EQ(scaler.scale(unsupported, 320, 200), nullptr);
  CVPixelBufferRelease(unsupported);
}

namespace {
  /**
   * @brief Run a captured frame through a zero-copy device.
   * @param device Initialized device.
   * @param source Captured frame.
   * @return Frame handed to the encoder, owned by the device.
   */
  CVPixelBufferRef encode_through(platf::nv12_zero_device &device, CVPixelBufferRef source) {
    CMVideoFormatDescriptionRef description = nullptr;
    CMSampleBufferRef sample = nullptr;
    CMSampleTimingInfo timing {kCMTimeInvalid, kCMTimeZero, kCMTimeInvalid};
    CMVideoFormatDescriptionCreateForImageBuffer(nullptr, source, &description);
    CMSampleBufferCreateReadyWithImageBuffer(nullptr, source, description, &timing, &sample);
    CVPixelBufferRef encoded = nullptr;
    {
      platf::av_img_t img;
      img.sample_buffer = std::make_shared<platf::av_sample_buf_t>(sample);
      img.pixel_buffer = std::make_shared<platf::av_pixel_buf_t>(img.sample_buffer->buf);
      if (device.convert(img) == 0) {
        encoded = reinterpret_cast<CVPixelBufferRef>(device.frame->data[3]);
      }
    }
    CFRelease(sample);
    CFRelease(description);
    return encoded;
  }
}  // namespace

TEST(MacosFrameScalerTest, ZeroCopyDeviceCapturesLargerDisplaysAtFullSize) {
  struct case_t {
    platf::pix_fmt_e pix_fmt;
    int display_width;
    int display_height;
    bool scales;
    OSType capture_format;
    OSType encoded_format;
  };

  const case_t cases[] {
    {platf::pix_fmt_e::nv12, 416, 260, true, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange},
    {platf::pix_fmt_e::nv12, 320, 200, false, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange},
    {platf::pix_fmt_e::nv24, 416, 260, true, kCVPixelFormatType_444YpCbCr10BiPlanarFullRange, kCVPixelFormatType_444YpCbCr8BiPlanarFullRange},
  };

  for (const auto &test_case : cases) {
    platf::nv12_zero_device device;
    int captured_width {};
    int captured_height {};
    ASSERT_EQ(device.init(nullptr, test_case.pix_fmt, true, [&](AVVideo *, int width, int height) {
      captured_width = width;
      captured_height = height;
    },
                          [](AVVideo *, int) {}),
              0);
    device.set_display_size(test_case.display_width, test_case.display_height);
    AVFrame *frame {av_frame_alloc()};
    frame->width = 320;
    frame->height = 200;
    ASSERT_EQ(device.set_frame(frame, nullptr), 0);

    EXPECT_EQ(device.scales_frames(), test_case.scales);
    EXPECT_EQ(captured_width, test_case.display_width);
    EXPECT_EQ(captured_height, test_case.display_height);

    CVPixelBufferRef source = make_frame(test_case.capture_format, captured_width, captured_height, 100);
    ASSERT_NE(source, nullptr);
    CVPixelBufferRef encoded = encode_through(device, source);
    ASSERT_NE(encoded, nullptr);
    EXPECT_EQ(CVPixelBufferGetWidth(encoded), 320u);
    EXPECT_EQ(CVPixelBufferGetHeight(encoded), 200u);
    EXPECT_EQ(CVPixelBufferGetPixelFormatType(encoded), test_case.encoded_format);
    EXPECT_EQ(encoded == source, !test_case.scales && test_case.pix_fmt == platf::pix_fmt_e::nv12);
    CVPixelBufferRelease(source);
  }
}

TEST(MacosFrameScalerTest, ZeroCopyDeviceFailsWhenAFrameCannotBeScaled) {
  platf::nv12_zero_device device;
  ASSERT_EQ(device.init(nullptr, platf::pix_fmt_e::nv12, true, [](AVVideo *, int, int) {}, [](AVVideo *, int) {}), 0);
  device.set_display_size(416, 260);
  AVFrame *frame {av_frame_alloc()};
  frame->width = 320;
  frame->height = 200;
  ASSERT_EQ(device.set_frame(frame, nullptr), 0);
  if (!device.scales_frames()) {
    GTEST_SKIP() << "Metal is not available";
  }

  CVPixelBufferRef unsupported = make_frame(kCVPixelFormatType_422YpCbCr8, 416, 260, 0);
  ASSERT_NE(unsupported, nullptr);
  EXPECT_EQ(encode_through(device, unsupported), nullptr);
  CVPixelBufferRelease(unsupported);
}

#endif
