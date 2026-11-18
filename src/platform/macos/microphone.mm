/**
 * @file src/platform/macos/microphone.mm
 * @brief Definitions for microphone capture on macOS.
 */
// standard includes
#include <chrono>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/macos/av_audio.h"

namespace platf {
  using namespace std::literals;

  /**
   * @brief macOS microphone capture device and audio format state.
   */
  struct av_mic_t: public mic_t {
    AVAudio *av_audio_capture {};  ///< AV audio capture.
    std::uint32_t channels {2};  ///< Interleaved channels per captured frame.
    std::uint64_t trimmed_bytes {};  ///< Old audio dropped to bound latency since the last report.
    std::uint32_t trims {};  ///< Number of backlog trims since the last report.
    std::uint32_t max_backlog_bytes {};  ///< Largest backlog seen since the last report.
    std::uint64_t reported_overflow_bytes {};  ///< Ring buffer overflow total at the last report.
    std::uint32_t reported_conversion_failures {};  ///< Conversion failure total at the last report.
    std::chrono::steady_clock::time_point next_report {std::chrono::steady_clock::now() + 10s};  ///< When drop statistics are logged next.

    ~av_mic_t() override {
      [av_audio_capture release];
    }

    /**
     * @brief Deliver a captured audio sample to Sunshine's audio pipeline.
     *
     * @param sample_in Sample in.
     * @return Capture status reported to the streaming pipeline.
     */
    capture_e sample(std::vector<float> &sample_in) override {
      const uint32_t neededBytes = static_cast<uint32_t>(sample_in.size() * sizeof(float));
      uint8_t *dst = reinterpret_cast<uint8_t *>(sample_in.data());

      uint32_t remaining = neededBytes;

      trim_backlog();

      while (remaining > 0) {
        uint32_t avail = 0;
        void *tail = TPCircularBufferTail(&av_audio_capture->audioSampleBuffer, &avail);

        if (avail == 0) {
          // Using 5 second timeout to prevent indefinite hanging
          dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, 5LL * NSEC_PER_SEC);
          if (dispatch_semaphore_wait(av_audio_capture->audioSemaphore, timeout) != 0) {
            BOOST_LOG(warning) << "Audio sample timeout - no audio data received within 5 seconds"sv;

            // Fill with silence and return to prevent hanging
            std::fill(sample_in.begin(), sample_in.end(), 0.0f);
            return capture_e::timeout;
          }
          continue;
        }

        const uint32_t toCopy = (avail < remaining) ? avail : remaining;
        std::memcpy(dst, tail, toCopy);

        TPCircularBufferConsume(&av_audio_capture->audioSampleBuffer, toCopy);

        dst += toCopy;
        remaining -= toCopy;
      }

      report_drops();
      return capture_e::ok;
    }

    /**
     * @brief Drop the oldest queued audio when the consumer fell behind, so the stream does not lag.
     */
    void trim_backlog() {
      const std::uint32_t frame_bytes = channels * sizeof(float);
      const std::uint32_t bytes_per_ms = 48 * frame_bytes;

      uint32_t backlog = 0;
      TPCircularBufferTail(&av_audio_capture->audioSampleBuffer, &backlog);
      max_backlog_bytes = std::max(max_backlog_bytes, backlog);

      const auto dropped = trimAudioBacklog(&av_audio_capture->audioSampleBuffer, kAudioMaxBacklogMs * bytes_per_ms, kAudioKeepBacklogMs * bytes_per_ms, frame_bytes);
      if (dropped > 0) {
        trimmed_bytes += dropped;
        ++trims;
      }
    }

    /**
     * @brief Log every 10 seconds how much audio was lost, if any, so crackling can be traced.
     */
    void report_drops() {
      const auto now = std::chrono::steady_clock::now();
      if (now < next_report) {
        return;
      }
      next_report = now + 10s;

      const std::uint32_t bytes_per_ms = 48 * channels * sizeof(float);
      const std::uint64_t overflow_total = av_audio_capture->overflowedBytes.load(std::memory_order_relaxed);
      const std::uint32_t conversion_total = av_audio_capture->conversionFailures.load(std::memory_order_relaxed);
      const auto overflow_ms = (overflow_total - reported_overflow_bytes) / bytes_per_ms;
      const auto conversion_failures = conversion_total - reported_conversion_failures;

      if (overflow_ms > 0 || trims > 0 || conversion_failures > 0) {
        BOOST_LOG(warning) << "Audio capture dropped audio in the last 10s: "sv
                           << overflow_ms << "ms on ring buffer overflow, "sv
                           << trimmed_bytes / bytes_per_ms << "ms in "sv << trims << " backlog trims, "sv
                           << conversion_failures << " failed conversions, max backlog "sv
                           << max_backlog_bytes / bytes_per_ms << "ms"sv;
      }

      reported_overflow_bytes = overflow_total;
      reported_conversion_failures = conversion_total;
      trimmed_bytes = 0;
      trims = 0;
      max_backlog_bytes = 0;
    }
  };

  /**
   * @brief macOS audio control state used to create microphone streams.
   */
  struct macos_audio_control_t: public audio_control_t {
    AVCaptureDevice *audio_capture_device {};  ///< Audio capture device.

  public:
    /**
     * @brief Update the sink value on the backend.
     *
     * @param sink Audio sink name to route or capture.
     * @return Status from updating sink.
     */
    int set_sink(const std::string &sink) override {
      BOOST_LOG(warning) << "audio_control_t::set_sink() unimplemented: "sv << sink;
      return 0;
    }

    /**
     * @brief Create a microphone capture stream for the requested layout.
     *
     * @param mapping Opus channel mapping table for the requested layout.
     * @param channels Number of audio channels in the stream.
     * @param sample_rate Audio sample rate in hertz.
     * @param frame_size Number of samples captured per audio frame.
     * @param continuous_audio Continuous audio.
     * @param host_audio_enabled Whether host playback should remain enabled during capture.
     * @return Microphone capture object for the requested audio layout.
     */
    std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous_audio, bool host_audio_enabled) override {
      auto mic = std::make_unique<av_mic_t>();
      mic->av_audio_capture = [[AVAudio alloc] init];

      // Set the host audio enabled flag from the stream configuration
      mic->av_audio_capture.hostAudioEnabled = host_audio_enabled ? YES : NO;
      mic->channels = static_cast<std::uint32_t>(channels);
      BOOST_LOG(info) << "Host audio playback during the stream: "sv << (host_audio_enabled ? "on"sv : "off"sv);

      if (config::audio.sink.empty()) {
        // Use macOS system-wide audio tap
        BOOST_LOG(info) << "Using macOS system audio tap for capture."sv;
        BOOST_LOG(info) << "Sample rate: "sv << sample_rate << ", Frame size: "sv << frame_size << ", Channels: "sv << channels;

        if ([mic->av_audio_capture setupSystemTap:sample_rate frameSize:frame_size channels:channels]) {
          BOOST_LOG(error) << "Failed to setup system audio tap."sv;
          return nullptr;
        }
      } else {
        // Use specified macOS audio sink
        const char *audio_sink = config::audio.sink.c_str();
        BOOST_LOG(info) << "Using configured audio sink "sv << audio_sink << " for capture."sv;

        if (!request_microphone_permission()) {
          return nullptr;
        }

        if ((audio_capture_device = [AVAudio findMicrophone:[NSString stringWithUTF8String:audio_sink]]) == nullptr) {
          BOOST_LOG(error) << "opening microphone '"sv << audio_sink << "' failed. Please set a valid input source in the Sunshine config."sv;
          BOOST_LOG(error) << "Available inputs:"sv;

          for (NSString *name in [AVAudio microphoneNames]) {
            BOOST_LOG(error) << "\t"sv << [name UTF8String];
          }

          return nullptr;
        }

        if ([mic->av_audio_capture setupMicrophone:audio_capture_device sampleRate:sample_rate frameSize:frame_size channels:channels]) {
          BOOST_LOG(error) << "Failed to setup microphone."sv;
          return nullptr;
        }
      }

      return mic;
    }

    bool is_sink_available(const std::string &sink) override {
      BOOST_LOG(warning) << "audio_control_t::is_sink_available() unimplemented: "sv << sink;
      return true;
    }

    /**
     * @brief Query host and virtual sink names available to Sunshine.
     *
     * @return Host and virtual sink names when the backend can report them.
     */
    std::optional<sink_t> sink_info() override {
      sink_t sink;

      return sink;
    }
  };

  std::unique_ptr<audio_control_t> audio_control() {
    return std::make_unique<macos_audio_control_t>();
  }
}  // namespace platf
