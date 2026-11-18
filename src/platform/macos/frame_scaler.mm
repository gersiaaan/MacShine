/**
 * @file src/platform/macos/frame_scaler.mm
 * @brief Definitions for downscaling captured frames on the GPU with a Lanczos filter.
 */
// platform includes
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>

// local includes
#include "src/platform/macos/frame_scaler.h"

namespace platf {

  namespace {
    /**
     * @brief Metal format of one plane of a pixel buffer.
     * @param buffer Pixel buffer.
     * @param plane Plane index, ignored for non-planar buffers.
     * @return Metal pixel format, or MTLPixelFormatInvalid when unsupported.
     */
    MTLPixelFormat plane_format(CVPixelBufferRef buffer, std::size_t plane) {
      switch (CVPixelBufferGetPixelFormatType(buffer)) {
        case kCVPixelFormatType_32BGRA:
          return MTLPixelFormatBGRA8Unorm;
        case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
        case kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange:
        case kCVPixelFormatType_444YpCbCr8BiPlanarFullRange:
          return plane == 0 ? MTLPixelFormatR8Unorm : MTLPixelFormatRG8Unorm;
        case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
        case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
        case kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange:
        case kCVPixelFormatType_444YpCbCr10BiPlanarFullRange:
          return plane == 0 ? MTLPixelFormatR16Unorm : MTLPixelFormatRG16Unorm;
        default:
          return MTLPixelFormatInvalid;
      }
    }
  }  // namespace

  /**
   * @brief Metal objects and the output pool of a frame_scaler_t.
   */
  struct frame_scaler_t::impl_t {
    id<MTLDevice> device {};  ///< GPU.
    id<MTLCommandQueue> queue {};  ///< Queue the scaling runs on.
    MPSImageLanczosScale *lanczos {};  ///< Lanczos filter.
    CVMetalTextureCacheRef texture_cache {};  ///< Wraps pixel buffer planes as textures.
    CVPixelBufferPoolRef pool {};  ///< Output buffers.
    OSType pool_format {};  ///< Pixel format of the pool's buffers.
    std::size_t pool_width {};  ///< Width of the pool's buffers.
    std::size_t pool_height {};  ///< Height of the pool's buffers.

    ~impl_t() {
      CVPixelBufferPoolRelease(pool);
      if (texture_cache) {
        CFRelease(texture_cache);
      }
      [lanczos release];
      [queue release];
      [device release];
    }

    /**
     * @brief Make sure the pool holds buffers of a format and size.
     * @param format Pixel format.
     * @param width Width in pixels.
     * @param height Height in pixels.
     * @return True when the pool is ready.
     */
    bool ensure_pool(OSType format, std::size_t width, std::size_t height) {
      if (pool && pool_format == format && pool_width == width && pool_height == height) {
        return true;
      }
      CVPixelBufferPoolRelease(pool);
      pool = nullptr;
      @autoreleasepool {
        NSDictionary *attributes = @{
        (NSString *) kCVPixelBufferPixelFormatTypeKey: @(format),
        (NSString *) kCVPixelBufferWidthKey: @(width),
        (NSString *) kCVPixelBufferHeightKey: @(height),
        (NSString *) kCVPixelBufferIOSurfacePropertiesKey: @ {},
          (NSString *) kCVPixelBufferMetalCompatibilityKey: @YES,
        };
        if (CVPixelBufferPoolCreate(nullptr, nullptr, (CFDictionaryRef) attributes, &pool) != kCVReturnSuccess) {
          pool = nullptr;
          return false;
        }
      }
      pool_format = format;
      pool_width = width;
      pool_height = height;
      return true;
    }

    /**
     * @brief Wrap one plane of a pixel buffer as a texture.
     * @param buffer Pixel buffer.
     * @param plane Plane index.
     * @return Texture reference that the caller releases, or nullptr on failure.
     */
    CVMetalTextureRef texture(CVPixelBufferRef buffer, std::size_t plane) {
      const bool planar {CVPixelBufferIsPlanar(buffer) != 0};
      const auto format {plane_format(buffer, plane)};
      if (format == MTLPixelFormatInvalid) {
        return nullptr;
      }
      CVMetalTextureRef result {};
      const auto width {planar ? CVPixelBufferGetWidthOfPlane(buffer, plane) : CVPixelBufferGetWidth(buffer)};
      const auto height {planar ? CVPixelBufferGetHeightOfPlane(buffer, plane) : CVPixelBufferGetHeight(buffer)};
      if (CVMetalTextureCacheCreateTextureFromImage(nullptr, texture_cache, buffer, nullptr, format, width, height, plane, &result) != kCVReturnSuccess) {
        return nullptr;
      }
      return result;
    }
  };

  frame_scaler_t::frame_scaler_t():
      impl {std::make_unique<impl_t>()} {
    impl->device = MTLCreateSystemDefaultDevice();
    if (!impl->device) {
      return;
    }
    impl->queue = [impl->device newCommandQueue];
    impl->lanczos = [[MPSImageLanczosScale alloc] initWithDevice:impl->device];
    // Repeat the edge pixels instead of reading black outside the frame, which darkens the border.
    impl->lanczos.edgeMode = MPSImageEdgeModeClamp;
    if (CVMetalTextureCacheCreate(nullptr, nullptr, impl->device, nullptr, &impl->texture_cache) != kCVReturnSuccess) {
      impl->texture_cache = nullptr;
    }
  }

  frame_scaler_t::~frame_scaler_t() = default;

  bool frame_scaler_t::ready() const {
    return impl->device && impl->queue && impl->lanczos && impl->texture_cache;
  }

  CVPixelBufferRef frame_scaler_t::scale(CVPixelBufferRef source, std::size_t width, std::size_t height) {
    if (!source || !ready() || width == 0 || height == 0 || plane_format(source, 0) == MTLPixelFormatInvalid) {
      return nullptr;
    }
    if (!impl->ensure_pool(CVPixelBufferGetPixelFormatType(source), width, height)) {
      return nullptr;
    }
    CVPixelBufferRef destination {};
    if (CVPixelBufferPoolCreatePixelBuffer(nullptr, impl->pool, &destination) != kCVReturnSuccess) {
      return nullptr;
    }
    CVBufferPropagateAttachments(source, destination);

    bool ok {true};
    @autoreleasepool {
      id<MTLCommandBuffer> commands = [impl->queue commandBuffer];
      const std::size_t planes {CVPixelBufferIsPlanar(source) ? CVPixelBufferGetPlaneCount(source) : 1};
      CVMetalTextureRef textures[4] {};
      for (std::size_t plane = 0; plane < planes && plane < 2 && ok; ++plane) {
        textures[plane * 2] = impl->texture(source, plane);
        textures[plane * 2 + 1] = impl->texture(destination, plane);
        if (!textures[plane * 2] || !textures[plane * 2 + 1]) {
          ok = false;
          break;
        }
        [impl->lanczos encodeToCommandBuffer:commands
                               sourceTexture:CVMetalTextureGetTexture(textures[plane * 2])
                          destinationTexture:CVMetalTextureGetTexture(textures[plane * 2 + 1])];
      }
      if (ok) {
        [commands commit];
        [commands waitUntilCompleted];
        ok = commands.status == MTLCommandBufferStatusCompleted;
      }
      for (auto *texture : textures) {
        if (texture) {
          CFRelease(texture);
        }
      }
    }
    CVMetalTextureCacheFlush(impl->texture_cache, 0);

    if (!ok) {
      CVPixelBufferRelease(destination);
      return nullptr;
    }
    return destination;
  }

  bool downscale_capture(int display_width, int display_height, int stream_width, int stream_height) {
    return display_width > stream_width && display_height > stream_height;
  }

}  // namespace platf
