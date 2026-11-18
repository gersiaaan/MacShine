/**
 * @file src/platform/macos/pixel_transfer.cpp
 * @brief Definitions for converting captured frames into the format VideoToolbox encodes.
 */
// local includes
#include "src/platform/macos/pixel_transfer.h"

namespace platf {

  pixel_transfer_t::pixel_transfer_t(OSType target_format):
      target_format {target_format} {
  }

  pixel_transfer_t::~pixel_transfer_t() {
    if (session) {
      VTPixelTransferSessionInvalidate(session);
      CFRelease(session);
    }
    CVPixelBufferPoolRelease(pool);
  }

  OSType pixel_transfer_t::format() const {
    return target_format;
  }

  bool pixel_transfer_t::ensure_pool(size_t width, size_t height) {
    if (pool && pool_width == width && pool_height == height) {
      return true;
    }
    CVPixelBufferPoolRelease(pool);
    pool = nullptr;

    const int format_value = static_cast<int>(target_format);
    const int width_value = static_cast<int>(width);
    const int height_value = static_cast<int>(height);
    CFNumberRef format_number = CFNumberCreate(nullptr, kCFNumberIntType, &format_value);
    CFNumberRef width_number = CFNumberCreate(nullptr, kCFNumberIntType, &width_value);
    CFNumberRef height_number = CFNumberCreate(nullptr, kCFNumberIntType, &height_value);
    CFDictionaryRef io_surface = CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *keys[] {kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferIOSurfacePropertiesKey};
    const void *values[] {format_number, width_number, height_number, io_surface};
    CFDictionaryRef attributes = CFDictionaryCreate(nullptr, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    const CVReturn status = CVPixelBufferPoolCreate(nullptr, nullptr, attributes, &pool);

    CFRelease(attributes);
    CFRelease(io_surface);
    CFRelease(height_number);
    CFRelease(width_number);
    CFRelease(format_number);

    if (status != kCVReturnSuccess) {
      pool = nullptr;
      return false;
    }
    pool_width = width;
    pool_height = height;
    return true;
  }

  CVPixelBufferRef pixel_transfer_t::convert(CVPixelBufferRef source) {
    if (!source) {
      return nullptr;
    }
    if (!session && VTPixelTransferSessionCreate(nullptr, &session) != noErr) {
      session = nullptr;
      return nullptr;
    }
    if (!ensure_pool(CVPixelBufferGetWidth(source), CVPixelBufferGetHeight(source))) {
      return nullptr;
    }

    CVPixelBufferRef destination = nullptr;
    if (CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &destination) != kCVReturnSuccess) {
      return nullptr;
    }
    // Same matrix, primaries and transfer function as the capture, so only the samples are converted.
    CVBufferPropagateAttachments(source, destination);
    if (VTPixelTransferSessionTransferImage(session, source, destination) != noErr) {
      CVPixelBufferRelease(destination);
      return nullptr;
    }
    return destination;
  }

}  // namespace platf
