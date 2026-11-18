/**
 * @file src/platform/macos/nv12_zero_device.cpp
 * @brief Definitions for NV12 zero copy device on macOS.
 */
// standard includes
#include <utility>

// local includes
#include "src/logging.h"
#include "src/platform/macos/av_img_t.h"
#include "src/platform/macos/capture_color.h"
#include "src/platform/macos/frame_scaler.h"
#include "src/platform/macos/nv12_zero_device.h"
#include "src/platform/macos/pixel_transfer.h"
#include "src/video.h"

extern "C" {
#include "libavutil/imgutils.h"
}

namespace platf {

  /**
   * @brief Release an FFmpeg frame allocated by the capture or conversion backend.
   */
  void free_frame(AVFrame *frame) {
    av_frame_free(&frame);
  }

  /**
   * @brief Release a backend buffer allocated for capture or conversion.
   *
   * @param opaque Opaque user pointer provided to the callback.
   * @param data Payload or state data to serialize, deserialize, or forward.
   */
  void free_buffer(void *opaque, uint8_t *data) {
    CVPixelBufferRelease((CVPixelBufferRef) data);
  }

  util::safe_ptr<AVFrame, &free_frame> av_frame;  ///< AV frame.

  nv12_zero_device::nv12_zero_device() = default;

  nv12_zero_device::~nv12_zero_device() = default;

  bool nv12_zero_device::converts_frames() const {
    return transfer != nullptr;
  }

  void nv12_zero_device::set_display_size(int width, int height) {
    display_width = width;
    display_height = height;
  }

  bool nv12_zero_device::scales_frames() const {
    return scaler != nullptr;
  }

  int nv12_zero_device::convert(platf::img_t &img) {
    auto *av_img = (av_img_t *) &img;

    CVPixelBufferRef captured = av_img->pixel_buffer->buf;
    // Retained: the captured frame itself, or its scaled copy.
    CVPixelBufferRef scaled = scaler ? scaler->scale(captured, stream_width, stream_height) : (CVPixelBufferRef) CFRetain(captured);
    if (!scaled) {
      BOOST_LOG(error) << "Failed to scale the captured frame to the stream size";
      return -1;
    }
    // Retained for the encoder: the frame, or its 4:4:4 conversion.
    CVPixelBufferRef encoded = scaled;
    if (transfer) {
      encoded = transfer->convert(scaled);
      CVPixelBufferRelease(scaled);
      if (!encoded) {
        BOOST_LOG(error) << "Failed to convert the captured frame for YUV 4:4:4 encoding";
        return -1;
      }
    }

    // Release any existing CVPixelBuffer previously retained for encoding
    av_buffer_unref(&av_frame->buf[0]);

    // Attach an AVBufferRef to this frame which will retain ownership of the CVPixelBuffer
    // until av_buffer_unref() is called (above) or the frame is freed with av_frame_free().
    //
    // The presence of the AVBufferRef allows FFmpeg to simply add a reference to the buffer
    // rather than having to perform a deep copy of the data buffers in avcodec_send_frame().
    av_frame->buf[0] = av_buffer_create((uint8_t *) encoded, 0, free_buffer, nullptr, 0);

    // Place a CVPixelBufferRef at data[3] as required by AV_PIX_FMT_VIDEOTOOLBOX
    av_frame->data[3] = (uint8_t *) encoded;

    return 0;
  }

  int nv12_zero_device::set_frame(AVFrame *new_frame, AVBufferRef *hw_frames_ctx) {
    this->frame = new_frame;

    av_frame.reset(new_frame);

    stream_width = new_frame->width;
    stream_height = new_frame->height;
    scaler.reset();
    if (downscale_capture(display_width, display_height, stream_width, stream_height)) {
      auto lanczos {std::make_unique<frame_scaler_t>()};
      if (lanczos->ready()) {
        scaler = std::move(lanczos);
        BOOST_LOG(info) << "Capturing " << display_width << 'x' << display_height << " and scaling to " << stream_width << 'x' << stream_height << " with Lanczos";
      }
    }

    if (scaler) {
      resolution_fn(this->display, display_width, display_height);
    } else {
      resolution_fn(this->display, stream_width, stream_height);
    }

    return 0;
  }

  int nv12_zero_device::init(AVVideo *capture_display, pix_fmt_e pix_fmt, bool full_range, resolution_fn_t set_resolution, const pixel_format_fn_t &pixel_format_fn) {
    const bool yuv444 {pix_fmt == pix_fmt_e::nv24};
    pixel_format_fn(capture_display, capture_color::pixel_format(pix_fmt == pix_fmt_e::p010, full_range, yuv444));
    if (yuv444) {
      transfer = std::make_unique<pixel_transfer_t>(capture_color::yuv444_encode_format(full_range));
    } else {
      transfer.reset();
    }

    this->display = capture_display;
    this->resolution_fn = std::move(set_resolution);

    hardware = true;

    return 0;
  }

}  // namespace platf
