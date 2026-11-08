/**
 * @file tests/unit/platform/macos/test_vt_session.cpp
 * @brief Tests for making VideoToolbox return each frame before the next one.
 */

#ifdef __APPLE__
  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <memory>

// lib includes
extern "C" {
  #include <libavcodec/avcodec.h>
  #include <libavutil/dict.h>
  #include <libavutil/frame.h>
  #include <libavutil/hwcontext.h>
}

  // local includes
  #include "src/platform/macos/vt_session.h"

namespace {
  struct codec_context_deleter {
    void operator()(AVCodecContext *ctx) const {
      avcodec_free_context(&ctx);
    }
  };

  struct frame_deleter {
    void operator()(AVFrame *frame) const {
      av_frame_free(&frame);
    }
  };

  struct packet_deleter {
    void operator()(AVPacket *packet) const {
      av_packet_free(&packet);
    }
  };

  using codec_context_t = std::unique_ptr<AVCodecContext, codec_context_deleter>;

  codec_context_t open_encoder(const char *name) {
    const AVCodec *codec {avcodec_find_encoder_by_name(name)};
    if (!codec) {
      return nullptr;
    }
    codec_context_t ctx {avcodec_alloc_context3(codec)};
    ctx->width = 2496;
    ctx->height = 1560;
    // Hardware frames, like the CVPixelBuffers Sunshine passes from ScreenCaptureKit.
    AVBufferRef *device {nullptr};
    if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0) != 0) {
      return nullptr;
    }
    AVBufferRef *frames {av_hwframe_ctx_alloc(device)};
    av_buffer_unref(&device);
    auto *frames_ctx {reinterpret_cast<AVHWFramesContext *>(frames->data)};
    frames_ctx->format = AV_PIX_FMT_VIDEOTOOLBOX;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width = ctx->width;
    frames_ctx->height = ctx->height;
    if (av_hwframe_ctx_init(frames) != 0) {
      av_buffer_unref(&frames);
      return nullptr;
    }
    ctx->hw_frames_ctx = frames;
    ctx->pix_fmt = AV_PIX_FMT_VIDEOTOOLBOX;
    ctx->sw_pix_fmt = AV_PIX_FMT_NV12;
    ctx->time_base = AVRational {1, 50};
    ctx->framerate = AVRational {50, 1};
    ctx->bit_rate = 5'000'000;
    ctx->gop_size = 600;
    ctx->max_b_frames = 0;
    // Like Sunshine: no AV_CODEC_FLAG_LOW_DELAY for VideoToolbox (see video::use_low_delay_flag).
    AVDictionary *options {nullptr};
    av_dict_set(&options, "realtime", "1", 0);
    av_dict_set(&options, "prio_speed", "1", 0);
    av_dict_set(&options, "max_ref_frames", "1", 0);
    const int opened {avcodec_open2(ctx.get(), codec, &options)};
    av_dict_free(&options);
    if (opened != 0) {
      return nullptr;
    }
    return ctx;
  }
}  // namespace

TEST(MacosVtSessionTest, VideoToolboxReturnsEachFrameBeforeTheNextOne) {
  auto ctx {open_encoder("hevc_videotoolbox")};
  if (!ctx) {
    GTEST_SKIP() << "hevc_videotoolbox is not available";
  }

  const auto completed_before {platf::vt_session::completed_frames()};
  std::unique_ptr<AVFrame, frame_deleter> frame {av_frame_alloc()};
  ASSERT_EQ(av_hwframe_get_buffer(ctx->hw_frames_ctx, frame.get(), 0), 0);
  std::unique_ptr<AVPacket, packet_deleter> packet {av_packet_alloc()};

  for (int frame_nr = 0; frame_nr < 5; ++frame_nr) {
    frame->pts = frame_nr;
    ASSERT_EQ(avcodec_send_frame(ctx.get(), frame.get()), 0);

    // The packet of this frame must be ready as soon as the frame is sent, like Sunshine reads it.
    ASSERT_EQ(avcodec_receive_packet(ctx.get(), packet.get()), 0) << "frame " << frame_nr << " was held back by the encoder";
    EXPECT_EQ(packet->pts, frame_nr);
    // Completing frames must not turn every frame into a keyframe.
    EXPECT_EQ((packet->flags & AV_PKT_FLAG_KEY) != 0, frame_nr == 0);
    av_packet_unref(packet.get());
    EXPECT_EQ(avcodec_receive_packet(ctx.get(), packet.get()), AVERROR(EAGAIN));
  }
  EXPECT_GE(platf::vt_session::completed_frames() - completed_before, 5u);
}

#endif
