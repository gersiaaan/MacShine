/**
 * @file src/platform/macos/nv12_zero_device.h
 * @brief Declarations for NV12 zero copy device on macOS.
 */
#pragma once

// standard includes
#include <memory>

// local includes
#include "src/platform/common.h"

struct AVFrame;

#ifdef __OBJC__
@class AVVideo;
#else
/**
 * @brief Opaque AVFoundation capture controller, defined in Objective-C.
 */
struct AVVideo;
#endif

namespace platf {
  class frame_scaler_t;
  class pixel_transfer_t;

  /**
   * @brief Release an FFmpeg frame allocated by the capture or conversion backend.
   *
   * @param frame Video or graphics frame being processed.
   */
  void free_frame(AVFrame *frame);

  /**
   * @brief macOS zero-copy encode device that forwards NV12 frames from AVFoundation to FFmpeg.
   *
   * YUV 4:4:4 frames are converted on the GPU first, because the capture only delivers them in 10-bit.
   */
  class nv12_zero_device: public avcodec_encode_device_t {
    AVVideo *display = nullptr;  ///< Borrowed AVFoundation controller, forward-declared to avoid FFmpeg name conflicts.

  public:
    nv12_zero_device();
    ~nv12_zero_device() override;

    // this function is used to set the resolution on an av_video object that we cannot
    // call directly because of namespace collisions between AVFoundation and FFMPEG
    /**
     * @brief Callback signature used to update the opaque AVFoundation display resolution.
     */
    using resolution_fn_t = std::function<void(AVVideo *display, int width, int height)>;
    resolution_fn_t resolution_fn;  ///< Callback stored for later AVFoundation resolution updates.
    /**
     * @brief Callback signature used to update the opaque AVFoundation pixel format.
     */
    using pixel_format_fn_t = std::function<void(AVVideo *display, int pixelFormat)>;

    /**
     * @brief Initialize zero-copy NV12 encoding for an AVFoundation display.
     *
     * For nv24 the capture is switched to 10-bit 4:4:4 and each frame is converted to 8-bit 4:4:4
     * in the stream range before it is encoded.
     *
     * @param capture_display Display object or identifier associated with the operation.
     * @param pix_fmt Sunshine pixel format to convert or allocate for.
     * @param full_range Whether frames are captured in full range instead of video range.
     * @param set_resolution Callback used to resize the AVFoundation capture output.
     * @param pixel_format_fn Pixel format.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(AVVideo *capture_display, pix_fmt_e pix_fmt, bool full_range, resolution_fn_t set_resolution, const pixel_format_fn_t &pixel_format_fn);

    /**
     * @brief Forward a captured AVFoundation frame to FFmpeg without copying.
     *
     * @param img Image or frame object to read from or populate.
     * @return Conversion status.
     */
    int convert(img_t &img) override;
    /**
     * @brief Attach frame resources used by the next conversion or encode operation.
     *
     * @param new_frame Video or graphics frame being processed.
     * @param hw_frames_ctx FFmpeg hardware frames context associated with the frame.
     * @return Status from updating frame.
     */
    int set_frame(AVFrame *new_frame, AVBufferRef *hw_frames_ctx) override;

    /**
     * @brief Check whether frames are converted before encoding.
     * @return True for YUV 4:4:4 streams.
     */
    bool converts_frames() const;

    /**
     * @brief Set the display's size in pixels, before set_frame.
     *
     * A display larger than the stream is then captured at its full size and scaled with a Lanczos
     * filter, which keeps thin text crisper than the scaling of the capture itself.
     *
     * @param width Display width in pixels.
     * @param height Display height in pixels.
     */
    void set_display_size(int width, int height);

    /**
     * @brief Check whether frames are scaled before encoding.
     * @return True when the display is captured larger than the stream.
     */
    bool scales_frames() const;

  private:
    util::safe_ptr<AVFrame, &free_frame> av_frame;
    std::unique_ptr<pixel_transfer_t> transfer;  ///< Converts 4:4:4 frames into the encoded format, if needed.
    std::unique_ptr<frame_scaler_t> scaler;  ///< Scales full-size captures to the stream size, if needed.
    int display_width {};  ///< Display width in pixels, 0 when unknown.
    int display_height {};  ///< Display height in pixels, 0 when unknown.
    int stream_width {};  ///< Width frames are scaled to.
    int stream_height {};  ///< Height frames are scaled to.
  };

}  // namespace platf
