/**
 * @file tests/unit/platform/macos/test_vt_session.cpp
 * @brief Tests for making VideoToolbox return each frame before the next one.
 */

#ifdef __APPLE__
  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <cstdint>
  #include <memory>
  #include <vector>

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

  codec_context_t open_encoder(const char *name, AVPixelFormat sw_format = AV_PIX_FMT_NV12) {
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
    frames_ctx->sw_format = sw_format;
    frames_ctx->width = ctx->width;
    frames_ctx->height = ctx->height;
    if (av_hwframe_ctx_init(frames) != 0) {
      av_buffer_unref(&frames);
      return nullptr;
    }
    ctx->hw_frames_ctx = frames;
    ctx->pix_fmt = AV_PIX_FMT_VIDEOTOOLBOX;
    ctx->sw_pix_fmt = sw_format;
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

  /**
   * @brief Read the profile and chroma format from the HEVC SPS in Annex B parameter sets.
   *
   * @param data Annex B data with start codes, such as a keyframe packet.
   * @param size Size of data.
   * @param profile_idc Receives general_profile_idc.
   * @param chroma_format_idc Receives chroma_format_idc.
   * @return True when an SPS was found.
   */
  bool read_hevc_sps(const uint8_t *data, int size, int &profile_idc, int &chroma_format_idc) {
    for (int i = 0; i + 3 < size; ++i) {
      if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1 || ((data[i + 3] >> 1) & 0x3f) != 33) {
        continue;
      }
      // Payload after the two-byte NAL header, without emulation prevention bytes.
      std::vector<uint8_t> rbsp;
      for (int j = i + 5; j < size; ++j) {
        if (j + 2 < size && data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 1) {
          break;
        }
        if (j >= 2 && data[j] == 3 && data[j - 1] == 0 && data[j - 2] == 0) {
          continue;
        }
        rbsp.push_back(data[j]);
      }
      std::size_t bit {0};
      auto read_bit = [&]() {
        const int value {bit / 8 < rbsp.size() ? (rbsp[bit / 8] >> (7 - bit % 8)) & 1 : 0};
        ++bit;
        return value;
      };
      auto read_bits = [&](int count) {
        int value {0};
        for (int n = 0; n < count; ++n) {
          value = (value << 1) | read_bit();
        }
        return value;
      };
      auto read_ue = [&]() {
        int zeros {0};
        while (read_bit() == 0 && zeros < 32) {
          ++zeros;
        }
        return (1 << zeros) - 1 + read_bits(zeros);
      };
      read_bits(4);  // sps_video_parameter_set_id
      const int max_sub_layers_minus1 {read_bits(3)};
      read_bits(1);  // sps_temporal_id_nesting_flag
      read_bits(3);  // general_profile_space, general_tier_flag
      profile_idc = read_bits(5);
      read_bits(32 + 48 + 8);  // compatibility flags, constraint flags, general_level_idc
      if (max_sub_layers_minus1 > 0) {
        return false;
      }
      read_ue();  // sps_seq_parameter_set_id
      chroma_format_idc = read_ue();
      return true;
    }
    return false;
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

TEST(MacosVtSessionTest, Hevc444FramesAreEncodedWithFullChromaResolution) {
  // Like Sunshine for YUV 4:4:4: NV24 frames and no profile (see video::select_hevc_profile).
  auto ctx {open_encoder("hevc_videotoolbox", AV_PIX_FMT_NV24)};
  if (!ctx) {
    GTEST_SKIP() << "hevc_videotoolbox cannot encode NV24 frames";
  }

  std::unique_ptr<AVFrame, frame_deleter> frame {av_frame_alloc()};
  ASSERT_EQ(av_hwframe_get_buffer(ctx->hw_frames_ctx, frame.get(), 0), 0);
  frame->pts = 0;
  ASSERT_EQ(avcodec_send_frame(ctx.get(), frame.get()), 0);
  std::unique_ptr<AVPacket, packet_deleter> packet {av_packet_alloc()};
  ASSERT_EQ(avcodec_receive_packet(ctx.get(), packet.get()), 0);

  // The SPS at the start of the keyframe says whether the chroma was subsampled.
  int profile_idc {-1};
  int chroma_format_idc {-1};
  ASSERT_TRUE(read_hevc_sps(packet->data, packet->size, profile_idc, chroma_format_idc));
  EXPECT_EQ(profile_idc, AV_PROFILE_HEVC_REXT);
  EXPECT_EQ(chroma_format_idc, 3) << "chroma_format_idc must be 4:4:4";
}

#endif
