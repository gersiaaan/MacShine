/**
 * @file tests/unit/test_client_mic.cpp
 * @brief Test src/client_mic.*.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

// lib includes
#include <opus/opus.h>

// local includes
#include <src/client_mic.h>
#include <src/config.h>
#include <src/platform/common.h>

namespace {
  /**
   * @brief Sink that records everything written to it.
   */
  class recording_sink_t: public platf::client_mic_sink_t {
  public:
    /**
     * @brief Create a sink appending to @p samples.
     *
     * @param samples Destination of the written samples.
     */
    explicit recording_sink_t(std::vector<float> &samples):
        samples {samples} {
    }

    void write(const float *data, std::size_t count) override {
      samples.insert(samples.end(), data, data + count);
    }

  private:
    std::vector<float> &samples;  ///< Destination of the written samples.
  };

  /**
   * @brief Opus encoder producing 20 ms mono frames of a sine tone, like the client does.
   */
  class tone_encoder_t {
  public:
    tone_encoder_t() {
      int err = OPUS_OK;
      encoder = opus_encoder_create(client_mic::SAMPLE_RATE, client_mic::CHANNELS, OPUS_APPLICATION_VOIP, &err);
      opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));
      opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(10));
    }

    ~tone_encoder_t() {
      opus_encoder_destroy(encoder);
    }

    /**
     * @brief Encode the next frame into a control-stream payload.
     *
     * @param sequence Sequence number to put in front of the frame.
     * @return Payload as sent by the client.
     */
    std::string next_payload(std::uint32_t sequence) {
      std::vector<float> pcm(client_mic::DEFAULT_FRAME_SAMPLES);
      for (auto &sample : pcm) {
        sample = 0.5f * std::sin(phase);
        phase += 2.0f * 3.14159265f * 440.0f / client_mic::SAMPLE_RATE;
      }

      unsigned char opus[400];
      const auto length = opus_encode_float(encoder, pcm.data(), client_mic::DEFAULT_FRAME_SAMPLES, opus, sizeof(opus));

      std::string payload(4, '\0');
      payload[0] = static_cast<char>(sequence & 0xFF);
      payload[1] = static_cast<char>((sequence >> 8) & 0xFF);
      payload[2] = static_cast<char>((sequence >> 16) & 0xFF);
      payload[3] = static_cast<char>((sequence >> 24) & 0xFF);
      payload.append(reinterpret_cast<char *>(opus), length);
      return payload;
    }

  private:
    OpusEncoder *encoder {};  ///< Encoder state.
    float phase {};  ///< Phase of the sine tone.
  };
}  // namespace

TEST(ClientMicPacketTest, ParsesLittleEndianSequenceAndOpusData) {
  const std::string payload {"\x04\x03\x02\x01opus", 8};
  const auto packet = client_mic::parse_packet(payload);
  ASSERT_TRUE(packet.has_value());
  EXPECT_EQ(packet->sequence, 0x01020304u);
  EXPECT_EQ(packet->opus, "opus");
}

TEST(ClientMicPacketTest, RejectsPayloadsWithoutAudio) {
  EXPECT_FALSE(client_mic::parse_packet(std::string_view {"\x01\x02\x03", 3}).has_value());
  EXPECT_FALSE(client_mic::parse_packet(std::string_view {"\x01\x02\x03\x04", 4}).has_value());
}

TEST(ClientMicPacketTest, OnlyASequenceNumberIsAStopPacket) {
  EXPECT_TRUE(client_mic::is_stop_packet(std::string_view {"\x01\x02\x03\x04", 4}));
  EXPECT_FALSE(client_mic::is_stop_packet(std::string_view {"\x01\x02\x03", 3}));
  EXPECT_FALSE(client_mic::is_stop_packet(std::string_view {"\x01\x02\x03\x04\x05", 5}));
}

TEST(ClientMicLevelTest, ZeroGainLeavesQuietSamplesAlone) {
  float samples[3] = {0.1f, -0.5f, 0.69f};
  client_mic::apply_level(samples, 3, 0.0f);
  EXPECT_FLOAT_EQ(samples[0], 0.1f);
  EXPECT_FLOAT_EQ(samples[1], -0.5f);
  EXPECT_FLOAT_EQ(samples[2], 0.69f);
}

TEST(ClientMicLevelTest, NegativeGainMakesItQuieter) {
  float samples[2] = {0.5f, -0.5f};
  client_mic::apply_level(samples, 2, -20.0f);
  EXPECT_NEAR(samples[0], 0.05f, 1e-6f);
  EXPECT_NEAR(samples[1], -0.05f, 1e-6f);
}

TEST(ClientMicLevelTest, LimiterKeepsPeaksBelowFullScale) {
  float samples[4] = {1.0f, -1.0f, 1.5f, -1.5f};
  client_mic::apply_level(samples, 4, 0.0f);
  for (const auto sample : samples) {
    EXPECT_LT(std::abs(sample), 1.0f);
    EXPECT_GT(std::abs(sample), client_mic::LIMITER_KNEE);
  }
  EXPECT_GT(samples[0], 0.0f);
  EXPECT_LT(samples[1], 0.0f);
  EXPECT_GT(samples[2], samples[0]) << "louder input stays louder after limiting";
}

TEST(ClientMicJitterBufferTest, PlaysSilenceUntilTheTargetIsQueued) {
  client_mic::jitter_buffer_t buffer {4, 16};
  const float input[3] = {1, 2, 3};
  float out[2] = {9, 9};

  buffer.push(input, 3);
  buffer.pull(out, 2);
  EXPECT_EQ(out[0], 0.0f);
  EXPECT_EQ(out[1], 0.0f);
  EXPECT_EQ(buffer.buffered(), 3u);

  const float more[1] = {4};
  buffer.push(more, 1);
  buffer.pull(out, 2);
  EXPECT_EQ(out[0], 1.0f);
  EXPECT_EQ(out[1], 2.0f);
}

TEST(ClientMicJitterBufferTest, PadsWithSilenceAndRePrimesAfterRunningDry) {
  client_mic::jitter_buffer_t buffer {2, 16};
  const float input[2] = {1, 2};
  float out[3] = {9, 9, 9};

  buffer.push(input, 2);
  buffer.pull(out, 3);
  EXPECT_EQ(out[0], 1.0f);
  EXPECT_EQ(out[1], 2.0f);
  EXPECT_EQ(out[2], 0.0f);

  const float one[1] = {5};
  buffer.push(one, 1);
  buffer.pull(out, 1);
  EXPECT_EQ(out[0], 0.0f) << "a single sample is below the target again";
}

TEST(ClientMicJitterBufferTest, DropsTheOldestAudioWhenFullAndWrapsAround) {
  client_mic::jitter_buffer_t buffer {1, 4};
  const float first[3] = {1, 2, 3};
  const float second[3] = {4, 5, 6};
  float out[4] = {};

  buffer.push(first, 3);
  buffer.push(second, 3);
  EXPECT_EQ(buffer.buffered(), 4u);
  EXPECT_EQ(buffer.dropped(), 2u);

  buffer.pull(out, 4);
  EXPECT_EQ(out[0], 3.0f);
  EXPECT_EQ(out[3], 6.0f);
}

TEST(ClientMicJitterBufferTest, KeepsOnlyTheNewestPartOfAnOversizedPush) {
  client_mic::jitter_buffer_t buffer {1, 2};
  const float input[5] = {1, 2, 3, 4, 5};
  float out[2] = {};

  buffer.push(input, 5);
  EXPECT_EQ(buffer.dropped(), 3u);
  buffer.pull(out, 2);
  EXPECT_EQ(out[0], 4.0f);
  EXPECT_EQ(out[1], 5.0f);
}

TEST(ClientMicDecoderTest, DecodesConsecutiveFrames) {
  client_mic::decoder_t decoder;
  tone_encoder_t encoder;
  ASSERT_TRUE(decoder.valid());

  std::vector<float> out;
  for (std::uint32_t sequence = 10; sequence < 13; ++sequence) {
    const auto payload = encoder.next_payload(sequence);
    ASSERT_TRUE(decoder.decode(*client_mic::parse_packet(payload), out));
    EXPECT_EQ(out.size(), static_cast<std::size_t>(client_mic::DEFAULT_FRAME_SAMPLES));
  }
  EXPECT_EQ(decoder.lost(), 0u);
  EXPECT_EQ(decoder.late(), 0u);
}

TEST(ClientMicDecoderTest, ConcealsShortGapsBeforeTheNextFrame) {
  client_mic::decoder_t decoder;
  tone_encoder_t encoder;
  std::vector<float> out;

  ASSERT_TRUE(decoder.decode(*client_mic::parse_packet(encoder.next_payload(0)), out));
  encoder.next_payload(1);
  encoder.next_payload(2);
  ASSERT_TRUE(decoder.decode(*client_mic::parse_packet(encoder.next_payload(3)), out));

  EXPECT_EQ(out.size(), 3u * client_mic::DEFAULT_FRAME_SAMPLES) << "two concealed frames plus the received one";
  EXPECT_EQ(decoder.lost(), 2u);
}

TEST(ClientMicDecoderTest, ResetsInsteadOfConcealingLongGaps) {
  client_mic::decoder_t decoder;
  tone_encoder_t encoder;
  std::vector<float> out;

  ASSERT_TRUE(decoder.decode(*client_mic::parse_packet(encoder.next_payload(0)), out));
  ASSERT_TRUE(decoder.decode(*client_mic::parse_packet(encoder.next_payload(100)), out));

  EXPECT_EQ(out.size(), static_cast<std::size_t>(client_mic::DEFAULT_FRAME_SAMPLES));
  EXPECT_EQ(decoder.lost(), 99u);
}

TEST(ClientMicDecoderTest, DropsLateAndDuplicatePackets) {
  client_mic::decoder_t decoder;
  tone_encoder_t encoder;
  std::vector<float> out;

  const auto first = encoder.next_payload(5);
  ASSERT_TRUE(decoder.decode(*client_mic::parse_packet(first), out));
  EXPECT_FALSE(decoder.decode(*client_mic::parse_packet(first), out));
  EXPECT_FALSE(decoder.decode(*client_mic::parse_packet(encoder.next_payload(4)), out));
  EXPECT_TRUE(out.empty());
  EXPECT_EQ(decoder.late(), 2u);
}

TEST(ClientMicDecoderTest, HandlesSequenceWrapAround) {
  client_mic::decoder_t decoder;
  tone_encoder_t encoder;
  std::vector<float> out;

  ASSERT_TRUE(decoder.decode(*client_mic::parse_packet(encoder.next_payload(0xFFFFFFFFu)), out));
  ASSERT_TRUE(decoder.decode(*client_mic::parse_packet(encoder.next_payload(0)), out));
  EXPECT_EQ(decoder.lost(), 0u);
  EXPECT_EQ(decoder.late(), 0u);
}

TEST(ClientMicDecoderTest, RejectsEmptyAndCorruptFrames) {
  client_mic::decoder_t decoder;
  std::vector<float> out;

  EXPECT_FALSE(decoder.decode(client_mic::packet_t {0, {}}, out));

  // An Opus TOC byte for code 3 without the frame-count byte is malformed.
  const std::string corrupt {"\x03", 1};
  EXPECT_FALSE(decoder.decode(client_mic::packet_t {0, corrupt}, out));
  EXPECT_TRUE(out.empty());
}

TEST(ClientMicReceiverTest, WritesDecodedAudioToTheSink) {
  std::vector<float> written;
  client_mic::receiver_t receiver {std::make_unique<recording_sink_t>(written)};
  tone_encoder_t encoder;

  receiver.receive(encoder.next_payload(0));
  receiver.receive(encoder.next_payload(1));
  receiver.receive(std::string_view {"\x01", 1});

  EXPECT_EQ(written.size(), 2u * client_mic::DEFAULT_FRAME_SAMPLES);
  float peak = 0;
  for (const auto sample : written) {
    peak = std::max(peak, std::abs(sample));
  }
  EXPECT_GT(peak, 0.1f) << "the tone should survive encoding";
}

TEST(ClientMicReceiverTest, DecodesWithoutASink) {
  client_mic::receiver_t receiver {nullptr};
  tone_encoder_t encoder;
  receiver.receive(encoder.next_payload(0));
}

TEST(ClientMicAllocTest, ReturnsNothingWhenDisabled) {
  const auto previous = config::audio.client_mic;
  config::audio.client_mic = false;
  EXPECT_EQ(client_mic::alloc(), nullptr);
  config::audio.client_mic = previous;
}
