/**
 * @file src/platform/macos/pixel_transfer.h
 * @brief Declarations for converting captured frames into the format VideoToolbox encodes.
 */
#pragma once

// platform includes
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

namespace platf {

  /**
   * @brief Converts frames into another pixel format on the GPU, reusing a pool of output buffers.
   *
   * ScreenCaptureKit delivers 4:4:4 frames only as 10-bit full range, while an 8-bit YUV 4:4:4 stream
   * is encoded from 8-bit frames in the stream range. The conversion keeps the full chroma resolution.
   */
  class pixel_transfer_t {
  public:
    /**
     * @brief Create a converter.
     * @param target_format CoreVideo pixel format of the converted frames.
     */
    explicit pixel_transfer_t(OSType target_format);

    pixel_transfer_t(const pixel_transfer_t &) = delete;
    pixel_transfer_t &operator=(const pixel_transfer_t &) = delete;

    ~pixel_transfer_t();

    /**
     * @brief Convert a frame.
     *
     * The converted frame keeps the source's color attachments, such as the YCbCr matrix, so only the
     * sample format changes.
     *
     * @param source Frame to convert.
     * @return Converted frame that the caller releases, or nullptr on failure.
     */
    CVPixelBufferRef convert(CVPixelBufferRef source);

    /**
     * @brief Get the pixel format frames are converted to.
     * @return CoreVideo pixel format.
     */
    OSType format() const;

  private:
    /**
     * @brief Make sure the output pool holds buffers of a size.
     * @param width Frame width in pixels.
     * @param height Frame height in pixels.
     * @return True when the pool is ready.
     */
    bool ensure_pool(size_t width, size_t height);

    OSType target_format;  ///< Pixel format of converted frames.
    VTPixelTransferSessionRef session {};  ///< Conversion session, created on first use.
    CVPixelBufferPoolRef pool {};  ///< Output buffers for the current size.
    size_t pool_width {};  ///< Width of the pool's buffers.
    size_t pool_height {};  ///< Height of the pool's buffers.
  };

}  // namespace platf
