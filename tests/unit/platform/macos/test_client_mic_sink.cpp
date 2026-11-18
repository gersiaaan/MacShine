/**
 * @file tests/unit/platform/macos/test_client_mic_sink.cpp
 * @brief Test src/platform/macos/client_mic_sink.*.
 */

// Only compile these tests on macOS
#ifdef __APPLE__

  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <vector>

  // local includes
  #include <src/client_mic.h>
  #include <src/config.h>
  #include <src/platform/common.h>
  #include <src/platform/macos/client_mic_sink.h>

namespace {
  /**
   * @brief Turn off switching the default input so tests never change the machine's audio setup.
   */
  class ClientMicSinkTest: public BaseTest {
  protected:
    void SetUp() override {
      BaseTest::SetUp();
      previous_default_input = config::audio.client_mic_default_input;
      config::audio.client_mic_default_input = false;
    }

    void TearDown() override {
      config::audio.client_mic_default_input = previous_default_input;
      BaseTest::TearDown();
    }

    bool previous_default_input {};  ///< Setting restored after the test.
  };
}  // namespace

TEST_F(ClientMicSinkTest, UnknownDeviceUidIsNotFound) {
  EXPECT_FALSE(platf::client_mic_device::find_by_uid("MacShine_Test_Missing_UID").has_value());
}

TEST_F(ClientMicSinkTest, DefaultInputCanBeReadAndSetToItself) {
  const auto input = platf::client_mic_device::default_input();
  if (!input) {
    GTEST_SKIP() << "no audio input on this machine";
  }
  EXPECT_TRUE(platf::client_mic_device::set_default_input(*input));
  EXPECT_EQ(platf::client_mic_device::default_input(), input);
}

TEST_F(ClientMicSinkTest, SinkOpensOnlyWithTheDriverInstalled) {
  const bool installed = platf::client_mic_device::find_by_uid(platf::client_mic_device::SENDER_UID).has_value();
  auto sink = platf::client_mic_sink(client_mic::SAMPLE_RATE, client_mic::CHANNELS);
  EXPECT_EQ(sink != nullptr, installed);

  if (sink) {
    const std::vector<float> silence(client_mic::DEFAULT_FRAME_SAMPLES);
    sink->write(silence.data(), silence.size());
  }
}

TEST_F(ClientMicSinkTest, SinkOpensWithAndWithoutVoiceIsolation) {
  if (!platf::client_mic_device::find_by_uid(platf::client_mic_device::SENDER_UID)) {
    GTEST_SKIP() << "MacShine audio driver not installed";
  }
  const auto previous = config::audio.client_mic_voice_isolation;
  const std::vector<float> tone(client_mic::DEFAULT_FRAME_SAMPLES, 0.25f);
  for (const bool isolation : {true, false}) {
    config::audio.client_mic_voice_isolation = isolation;
    auto sink = platf::client_mic_sink(client_mic::SAMPLE_RATE, client_mic::CHANNELS);
    ASSERT_NE(sink, nullptr);
    sink->write(tone.data(), tone.size());
  }
  config::audio.client_mic_voice_isolation = previous;
}

TEST_F(ClientMicSinkTest, AllocFollowsTheDriver) {
  const auto previous = config::audio.client_mic;
  config::audio.client_mic = true;
  const bool installed = platf::client_mic_device::find_by_uid(platf::client_mic_device::SENDER_UID).has_value();
  EXPECT_EQ(client_mic::alloc() != nullptr, installed);
  config::audio.client_mic = previous;
}

#endif
