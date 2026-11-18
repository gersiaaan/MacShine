/**
 * @file src/platform/macos/frame_scaler.h
 * @brief Declarations for downscaling captured frames on the GPU with a Lanczos filter.
 */
#pragma once

// standard includes
#include <cstddef>
#include <memory>

// platform includes
#include <CoreVideo/CoreVideo.h>

namespace platf {

  /**
   * @brief Scales frames with Metal's Lanczos filter, keeping their pixel format.
   *
   * A HiDPI virtual display larger than the stream, such as 2496x1560 for a 1920x1200 client, is
   * captured at its full size and scaled here. Lanczos keeps thin text crisper than the bilinear
   * scaling of the capture itself. Each plane of a YCbCr frame is scaled on its own.
   */
  class frame_scaler_t {
  public:
    frame_scaler_t();
    ~frame_scaler_t();

    frame_scaler_t(const frame_scaler_t &) = delete;
    frame_scaler_t &operator=(const frame_scaler_t &) = delete;

    /**
     * @brief Check whether the GPU resources could be created.
     * @return True when frames can be scaled.
     */
    bool ready() const;

    /**
     * @brief Scale a frame.
     *
     * The scaled frame has the source's pixel format and color attachments.
     *
     * @param source Frame to scale, backed by an IOSurface.
     * @param width Width of the scaled frame.
     * @param height Height of the scaled frame.
     * @return Scaled frame that the caller releases, or nullptr on failure.
     */
    CVPixelBufferRef scale(CVPixelBufferRef source, std::size_t width, std::size_t height);

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl;  ///< Metal state, kept out of the header for C++ users.
  };

  /**
   * @brief Check whether a captured display should be scaled by frame_scaler_t.
   *
   * @param display_width Display width in pixels.
   * @param display_height Display height in pixels.
   * @param stream_width Stream width in pixels.
   * @param stream_height Stream height in pixels.
   * @return True when the display is larger than the stream in both directions.
   */
  bool downscale_capture(int display_width, int display_height, int stream_width, int stream_height);

}  // namespace platf
