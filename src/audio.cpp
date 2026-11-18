/**
 * @file src/audio.cpp
 * @brief Definitions for audio capture and encoding.
 */
// standard includes
#include <algorithm>
#include <array>
#include <filesystem>
#include <system_error>
#include <thread>

// lib includes
#include <opus/opus_multistream.h>

// local includes
#include "client_mic_aec.h"
#include "audio.h"
#include "config.h"
#include "file_handler.h"
#include "globals.h"
#include "logging.h"
#include "platform/common.h"
#include "thread_safe.h"
#include "utility.h"

namespace audio {
  using namespace std::literals;
  /**
   * @brief Owning pointer for an Opus multistream encoder.
   */
  using opus_t = util::safe_ptr<OpusMSEncoder, opus_multistream_encoder_destroy>;
  /**
   * @brief Shared queue carrying captured PCM sample buffers to the encoder thread.
   */
  using sample_queue_t = std::shared_ptr<safe::queue_t<std::vector<float>>>;

  static int start_audio_control(audio_ctx_t &ctx);
  static void stop_audio_control(audio_ctx_t &);
  static void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params);

  /**
   * @brief Select the Opus stream configuration for a channel count and quality tier.
   *
   * @param channels Number of audio channels in the stream.
   * @param quality Whether the high-quality Opus layout should be selected.
   * @return Index into `stream_configs` for the requested layout.
   */
  int map_stream(int channels, bool quality);

  constexpr auto SAMPLE_RATE = 48000;  ///< Audio sample rate in hertz required by Opus.

  // NOTE: If you adjust the bitrates listed here, make sure to update the
  // corresponding bitrate adjustment logic in rtsp_stream::cmd_announce()
  /**
   * @brief Opus stream layouts and bitrates advertised to clients.
   */
  const std::array<opus_stream_config_t, MAX_STREAM_CONFIG> stream_configs {{
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo.data(),
      96000,
    },
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo.data(),
      512000,
    },
    {
      SAMPLE_RATE,
      6,
      4,
      2,
      platf::speaker::map_surround51.data(),
      256000,
    },
    {
      SAMPLE_RATE,
      6,
      6,
      0,
      platf::speaker::map_surround51.data(),
      1536000,
    },
    {
      SAMPLE_RATE,
      8,
      5,
      3,
      platf::speaker::map_surround71.data(),
      450000,
    },
    {
      SAMPLE_RATE,
      8,
      8,
      0,
      platf::speaker::map_surround71.data(),
      2048000,
    },
  }};

  void delay_line_t::push(clock::time_point captured_at, std::vector<float> &&samples) {
    queue.emplace_back(captured_at, std::move(samples));
  }

  std::vector<std::vector<float>> delay_line_t::pop_ready(clock::time_point now, std::chrono::milliseconds delay) {
    std::vector<std::vector<float>> ready;
    while (!queue.empty() && queue.front().first + delay <= now) {
      ready.emplace_back(std::move(queue.front().second));
      queue.pop_front();
    }
    return ready;
  }

  std::size_t delay_line_t::size() const {
    return queue.size();
  }

  int read_audio_delay_ms(std::string_view file_content) {
    auto vars = config::parse_config(file_content);
    auto it = vars.find("audio_delay");
    if (it == vars.end()) {
      return 0;
    }
    try {
      return std::clamp(std::stoi(it->second), 0, config::MAX_AUDIO_DELAY_MS);
    } catch (...) {
      return 0;
    }
  }

  namespace {
    /**
     * @brief Follows the audio_delay setting in the config file so it can be tuned during a stream.
     */
    class live_audio_delay_t {
    public:
      /**
       * @brief Start from the delay loaded at startup.
       *
       * @param config_file Path of the Sunshine config file to follow.
       * @param initial_ms Delay loaded at startup.
       */
      live_audio_delay_t(std::string config_file, int initial_ms):
          config_file {std::move(config_file)},
          delay {initial_ms} {
        std::error_code ec;
        last_write = std::filesystem::last_write_time(this->config_file, ec);
      }

      /**
       * @brief Current delay, re-reading the config file at most once a second when it changed.
       *
       * @param now Current time.
       * @return Delay to apply.
       */
      std::chrono::milliseconds get(delay_line_t::clock::time_point now) {
        if (now >= next_check && !config_file.empty()) {
          next_check = now + std::chrono::seconds {1};
          std::error_code ec;
          auto write = std::filesystem::last_write_time(config_file, ec);
          if (!ec && write != last_write) {
            last_write = write;
            auto updated = read_audio_delay_ms(file_handler::read_file(config_file.c_str()));
            if (updated != delay.count()) {
              BOOST_LOG(info) << "Audio delay changed to "sv << updated << "ms"sv;
              delay = std::chrono::milliseconds {updated};
            }
          }
        }
        return delay;
      }

    private:
      std::string config_file;  ///< Config file being followed.
      std::chrono::milliseconds delay;  ///< Delay in effect.
      std::filesystem::file_time_type last_write {};  ///< Modification time last read.
      delay_line_t::clock::time_point next_check {};  ///< When the file is checked next.
    };
  }  // namespace

  /**
   * @brief Encode captured PCM samples into Opus packets on the audio worker thread.
   *
   * @param samples Queue of captured PCM sample buffers to encode.
   * @param config Audio stream settings negotiated with the client.
   * @param channel_data Stream session carried with each encoded packet.
   */
  void encodeThread(sample_queue_t samples, config_t config, stream::session_t *channel_data) {
    auto packets = mail::man->queue<packet_t>(mail::audio_packets);
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    // Encoding takes place on this thread
    platf::set_thread_name("audio::encode");
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    opus_t opus {opus_multistream_encoder_create(
      stream.sampleRate,
      stream.channelCount,
      stream.streams,
      stream.coupledStreams,
      stream.mapping,
      OPUS_APPLICATION_RESTRICTED_LOWDELAY,
      nullptr
    )};

    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_BITRATE(stream.bitrate));
    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_VBR(0));

    BOOST_LOG(info) << "Opus initialized: "sv << stream.sampleRate / 1000 << " kHz, "sv
                    << stream.channelCount << " channels, "sv
                    << stream.bitrate / 1000 << " kbps (total), LOWDELAY"sv;

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    while (auto sample = samples->pop()) {
      buffer_t packet {1400};

      int bytes = opus_multistream_encode_float(opus.get(), sample->data(), frame_size, std::begin(packet), (opus_int32) packet.size());
      if (bytes < 0) {
        BOOST_LOG(error) << "Couldn't encode audio: "sv << opus_strerror(bytes);
        packets->stop();

        return;
      }

      packet.fake_resize(bytes);
      packets->raise(channel_data, std::move(packet));
    }
  }

  /**
   * @brief Run the capture loop for this backend.
   */
  void capture(safe::mail_t mail, config_t config, stream::session_t *channel_data) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);
    if (!config::audio.stream) {
      shutdown_event->view();
      return;
    }
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    auto ref = get_audio_ctx_ref();
    if (!ref) {
      return;
    }

    auto init_failure_fg = util::fail_guard([&shutdown_event]() {
      BOOST_LOG(error) << "Unable to initialize audio capture. The stream will not have audio."sv;

      // Wait for shutdown to be signalled if we fail init.
      // This allows streaming to continue without audio.
      shutdown_event->view();
    });

    auto &control = ref->control;
    if (!control) {
      return;
    }

    // Order of priority:
    // 1. Virtual sink
    // 2. Audio sink
    // 3. Host
    std::string *sink = &ref->sink.host;
    if (!config::audio.sink.empty()) {
      sink = &config::audio.sink;
    }

    // Prefer the virtual sink if host playback is disabled or there's no other sink
    if (ref->sink.null && (!config.flags[config_t::HOST_AUDIO] || sink->empty())) {
      auto &null = *ref->sink.null;
      switch (stream.channelCount) {
        case 2:
          sink = &null.stereo;
          break;
        case 6:
          sink = &null.surround51;
          break;
        case 8:
          sink = &null.surround71;
          break;
      }
    }

    // Only the first to start a session may change the default sink
    if (!ref->sink_flag->exchange(true, std::memory_order_acquire)) {
      // If the selected sink is different than the current one, change sinks.
      ref->restore_sink = ref->sink.host != *sink;
      if (ref->restore_sink) {
        if (control->set_sink(*sink)) {
          return;
        }
      }
    }

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    bool host_audio = config.flags[config_t::HOST_AUDIO];
    bool continuous_audio = config.flags[config_t::CONTINUOUS_AUDIO];
    auto mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size, continuous_audio, host_audio);
    if (!mic) {
      return;
    }

    // Audio is initialized, so we don't want to print the failure message
    init_failure_fg.disable();

    // Capture takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    auto samples = std::make_shared<sample_queue_t::element_type>(30);
    std::jthread thread {encodeThread, samples, config, channel_data};

    auto fg = util::fail_guard([&samples, &thread, &shutdown_event]() {
      samples->stop();
      thread.join();

      shutdown_event->view();
    });

    int samples_per_frame = frame_size * stream.channelCount;

    live_audio_delay_t audio_delay {config::sunshine.config_file, config::audio.delay_ms};
    delay_line_t delay_line;
    if (config::audio.delay_ms > 0) {
      BOOST_LOG(info) << "Delaying audio by "sv << config::audio.delay_ms << "ms"sv;
    }

    while (!shutdown_event->peek()) {
      std::vector<float> sample_buffer;
      sample_buffer.resize(samples_per_frame);

      auto status = mic->sample(sample_buffer);
      switch (status) {
        case platf::capture_e::ok:
          break;
        case platf::capture_e::timeout:
          continue;
        case platf::capture_e::reinit:
          BOOST_LOG(info) << "Reinitializing audio capture"sv;
          mic.reset();
          do {
            mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size, continuous_audio, host_audio);
            if (!mic) {
              BOOST_LOG(warning) << "Couldn't re-initialize audio input"sv;
            }
          } while (!mic && !shutdown_event->view(5s));
          continue;
        default:
          return;
      }

      const auto now = delay_line_t::clock::now();
      const auto delay = audio_delay.get(now);
      if (delay.count() == 0 && delay_line.size() == 0) {
        // What goes to the client is the echo canceller's reference for the client's microphone.
        client_mic::stream_reference().push(sample_buffer.data(), sample_buffer.size(), stream.channelCount);
        samples->raise(std::move(sample_buffer));
        continue;
      }

      delay_line.push(now, std::move(sample_buffer));
      for (auto &ready : delay_line.pop_ready(now, delay)) {
        client_mic::stream_reference().push(ready.data(), ready.size(), stream.channelCount);
        samples->raise(std::move(ready));
      }
    }
  }

  audio_ctx_ref_t get_audio_ctx_ref() {
    static auto control_shared {safe::make_shared<audio_ctx_t>(start_audio_control, stop_audio_control)};
    return control_shared.ref();
  }

  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx) {
    if (!ctx.control) {
      return false;
    }

    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (sink.empty()) {
      return false;
    }

    return ctx.control->is_sink_available(sink);
  }

  /**
   * @brief Select the Opus stream configuration for a channel count and quality tier.
   */
  int map_stream(int channels, bool quality) {
    int shift = quality ? 1 : 0;
    switch (channels) {
      case 2:
        return STEREO + shift;
      case 6:
        return SURROUND51 + shift;
      case 8:
        return SURROUND71 + shift;
    }
    return STEREO;
  }

  int start_audio_control(audio_ctx_t &ctx) {
    auto fg = util::fail_guard([]() {
      BOOST_LOG(warning) << "There will be no audio"sv;
    });

    ctx.sink_flag = std::make_unique<std::atomic_bool>(false);

    // The default sink has not been replaced yet.
    ctx.restore_sink = false;

    if (!(ctx.control = platf::audio_control())) {
      return 0;
    }

    auto sink = ctx.control->sink_info();
    if (!sink) {
      // Let the calling code know it failed
      ctx.control.reset();
      return 0;
    }

    ctx.sink = std::move(*sink);

    fg.disable();
    return 0;
  }

  void stop_audio_control(audio_ctx_t &ctx) {
    // restore audio-sink if applicable
    if (!ctx.restore_sink) {
      return;
    }

    // Change back to the host sink, unless there was none
    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (!sink.empty()) {
      // Best effort, it's allowed to fail
      ctx.control->set_sink(sink);
    }
  }

  void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params) {
    stream.channelCount = params.channelCount;
    stream.streams = params.streams;
    stream.coupledStreams = params.coupledStreams;
    stream.mapping = params.mapping.data();
  }
}  // namespace audio
