/**
 * @file tests/unit/test_client_mic_aec.cpp
 * @brief Test src/client_mic_aec.*.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <cmath>
#include <complex>
#include <random>
#include <vector>

// local includes
#include <src/client_mic_aec.h>

namespace {
  /**
   * @brief Mean square of a range of samples.
   *
   * @param s Samples.
   * @param from First index.
   * @param to One past the last index.
   * @return Mean square.
   */
  double mean_square(const std::vector<float> &s, std::size_t from, std::size_t to) {
    double sum = 0;
    for (auto i = from; i < to; ++i) {
      sum += s[i] * s[i];
    }
    return sum / static_cast<double>(to - from);
  }

  /**
   * @brief Feeds a reference and an echoing microphone in 20 ms frames, like a stream does.
   *
   * @param aec Canceller under test.
   * @param reference Reference buffer the canceller reads.
   * @param ref Reference signal.
   * @param mic Microphone signal, processed in place.
   * @param lead Reference frames pushed before the first microphone frame.
   */
  void run(client_mic::echo_canceller_t &aec, client_mic::reference_buffer_t &reference, const std::vector<float> &ref, std::vector<float> &mic, std::size_t lead) {
    constexpr std::size_t frame = 960;
    std::size_t pushed = 0;
    for (std::size_t f = 0; f < lead && pushed < ref.size(); ++f, pushed += frame) {
      reference.push(ref.data() + pushed, frame, 1);
    }
    for (std::size_t m = 0; m + frame <= mic.size(); m += frame) {
      if (pushed + frame <= ref.size()) {
        reference.push(ref.data() + pushed, frame, 1);
        pushed += frame;
      }
      aec.process(mic.data() + m, frame);
    }
  }
}  // namespace

TEST(ClientMicFftTest, ForwardThenInverseRestoresTheSignal) {
  std::vector<std::complex<float>> data(16);
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = {static_cast<float>(i % 5), 0.0f};
  }
  const auto original = data;
  client_mic::fft(data, false);
  client_mic::fft(data, true);
  for (std::size_t i = 0; i < data.size(); ++i) {
    EXPECT_NEAR(data[i].real(), original[i].real(), 1e-4f);
    EXPECT_NEAR(data[i].imag(), 0.0f, 1e-4f);
  }
}

TEST(ClientMicFftTest, ConstantSignalHasOnlyADcBin) {
  std::vector<std::complex<float>> data(8, {1.0f, 0.0f});
  client_mic::fft(data, false);
  EXPECT_NEAR(data[0].real(), 8.0f, 1e-5f);
  for (std::size_t i = 1; i < data.size(); ++i) {
    EXPECT_NEAR(std::abs(data[i]), 0.0f, 1e-5f);
  }
}

TEST(ClientMicReferenceTest, DownmixesAndReadsBackByPosition) {
  client_mic::reference_buffer_t reference;
  const float stereo[4] = {1.0f, 0.0f, 0.5f, 0.5f};
  reference.push(stereo, 4, 2);
  EXPECT_EQ(reference.write_position(), 2u);

  float out[4] = {9, 9, 9, 9};
  reference.read(0, out, 4);
  EXPECT_FLOAT_EQ(out[0], 0.5f);
  EXPECT_FLOAT_EQ(out[1], 0.5f);
  EXPECT_FLOAT_EQ(out[2], 0.0f) << "not yet written reads as silence";

  reference.push(stereo, 4, 0);
  EXPECT_EQ(reference.write_position(), 2u) << "no channels pushes nothing";

  reference.clear();
  EXPECT_EQ(reference.write_position(), 0u);
}

TEST(ClientMicReferenceTest, ForgetsSamplesOlderThanItsCapacity) {
  client_mic::reference_buffer_t reference;
  std::vector<float> ones(client_mic::AEC_REFERENCE_CAPACITY + 10, 1.0f);
  reference.push(ones.data(), ones.size(), 1);
  float out[2] = {9, 9};
  reference.read(0, out, 2);
  EXPECT_FLOAT_EQ(out[0], 0.0f);
  reference.read(ones.size() - 1, out, 1);
  EXPECT_FLOAT_EQ(out[0], 1.0f);
}

TEST(ClientMicEchoCancellerTest, RemovesADelayedEcho) {
  client_mic::reference_buffer_t reference;
  client_mic::echo_canceller_t aec {reference};

  constexpr std::size_t seconds = 8;
  std::mt19937 rng {42};
  std::normal_distribution<float> noise {0.0f, 0.1f};
  std::vector<float> ref(48000 * seconds);
  for (auto &x : ref) {
    x = noise(rng);
  }

  // The microphone hears the stream 150 ms later through a small "room": two taps.
  constexpr std::size_t delay = 7200;
  std::vector<float> mic(ref.size());
  for (std::size_t i = delay + 40; i < mic.size(); ++i) {
    mic[i] = 0.5f * ref[i - delay] + 0.2f * ref[i - delay - 40];
  }
  const auto echo_power = mean_square(mic, mic.size() - 48000, mic.size());

  run(aec, reference, ref, mic, 0);

  const auto residual_power = mean_square(mic, mic.size() - 48000, mic.size());
  const double erle_db = 10 * std::log10(echo_power / residual_power);
  EXPECT_GT(erle_db, 15.0) << "echo return loss enhancement in the last second";
}

TEST(ClientMicEchoCancellerTest, KeepsTheVoiceWhileRemovingTheEcho) {
  client_mic::reference_buffer_t reference;
  client_mic::echo_canceller_t aec {reference};

  std::mt19937 rng {7};
  std::normal_distribution<float> noise {0.0f, 0.1f};
  std::vector<float> ref(48000 * 8);
  for (auto &x : ref) {
    x = noise(rng);
  }
  constexpr std::size_t delay = 4800;
  std::vector<float> mic(ref.size());
  std::vector<float> voice(ref.size());
  for (std::size_t i = delay; i < mic.size(); ++i) {
    mic[i] = 0.4f * ref[i - delay];
  }
  // The user talks during the last two seconds, after the filter had time to learn the echo.
  for (std::size_t i = mic.size() - 96000; i < mic.size(); ++i) {
    voice[i] = 0.2f * std::sin(static_cast<float>(i) * 0.03f);
    mic[i] += voice[i];
  }

  run(aec, reference, ref, mic, 0);

  // Compare the output with the voice at the canceller's output lag (under one block).
  double best = 0;
  for (std::size_t lag = 0; lag <= client_mic::AEC_BLOCK; ++lag) {
    double residual = 0;
    double voice_power = 0;
    for (std::size_t i = mic.size() - 48000; i < mic.size(); ++i) {
      const double diff = mic[i] - voice[i - lag];
      residual += diff * diff;
      voice_power += voice[i - lag] * voice[i - lag];
    }
    best = std::max(best, 10 * std::log10(voice_power / residual));
  }
  EXPECT_GT(best, 10.0) << "voice kept, echo mostly gone";
}

TEST(ClientMicEchoCancellerTest, KeepsTheVoiceWhenTheStreamIsSilent) {
  client_mic::reference_buffer_t reference;
  client_mic::echo_canceller_t aec {reference};

  std::vector<float> silence(48000, 0.0f);
  std::vector<float> mic(48000);
  for (std::size_t i = 0; i < mic.size(); ++i) {
    mic[i] = 0.3f * std::sin(static_cast<float>(i) * 0.05f);
  }
  const auto voice_power = mean_square(mic, 0, mic.size() - 1024);

  run(aec, reference, silence, mic, 10);

  // Output lags by one block at most; compare power over the steady part.
  const auto out_power = mean_square(mic, 2048, mic.size());
  EXPECT_NEAR(out_power / voice_power, 1.0, 0.05);
}

TEST(ClientMicEchoCancellerTest, PassesTheMicrophoneThroughWithoutAnyStream) {
  client_mic::reference_buffer_t reference;
  client_mic::echo_canceller_t aec {reference};
  std::vector<float> mic(4800, 0.25f);
  for (std::size_t m = 0; m < mic.size(); m += 960) {
    aec.process(mic.data() + m, 960);
  }
  EXPECT_FLOAT_EQ(mic.back(), 0.25f);
  EXPECT_EQ(aec.resyncs(), 0u);
}

TEST(ClientMicEchoCancellerTest, RealignsWhenTheMicrophoneFallsFarBehind) {
  client_mic::reference_buffer_t reference;
  client_mic::echo_canceller_t aec {reference};
  std::vector<float> ref(48000, 0.1f);
  reference.push(ref.data(), ref.size(), 1);

  std::vector<float> mic(960, 0.0f);
  aec.process(mic.data(), mic.size());
  EXPECT_EQ(aec.resyncs(), 0u);

  // A second of stream with no microphone: the cursor is now far behind.
  reference.push(ref.data(), ref.size(), 1);
  aec.process(mic.data(), mic.size());
  EXPECT_EQ(aec.resyncs(), 1u);
}

TEST(ClientMicStreamReferenceTest, IsOneSharedBuffer) {
  EXPECT_EQ(&client_mic::stream_reference(), &client_mic::stream_reference());
}
