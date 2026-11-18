/**
 * @file src/client_mic.cpp
 * @brief Definitions for receiving the client's microphone and playing it into the host's virtual microphone.
 */
// standard includes
#include <algorithm>
#include <cmath>

// lib includes
#include <opus/opus.h>

// local includes
#include "client_mic.h"
#include "config.h"
#include "logging.h"
#include "platform/common.h"

using namespace std::literals;

namespace client_mic {
  std::optional<packet_t> parse_packet(std::string_view payload) {
    if (payload.size() <= sizeof(std::uint32_t)) {
      return std::nullopt;
    }

    const auto *bytes = reinterpret_cast<const std::uint8_t *>(payload.data());
    const std::uint32_t sequence = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
    return packet_t {sequence, payload.substr(sizeof(std::uint32_t))};
  }

  bool is_stop_packet(std::string_view payload) {
    return payload.size() == sizeof(std::uint32_t);
  }

  void apply_level(float *samples, std::size_t count, float gain_db) {
    const float gain = std::pow(10.0f, gain_db / 20.0f);
    constexpr float headroom = 1.0f - LIMITER_KNEE;
    for (std::size_t i = 0; i < count; ++i) {
      const float x = samples[i] * gain;
      const float magnitude = std::abs(x);
      if (magnitude > LIMITER_KNEE) {
        const float limited = LIMITER_KNEE + headroom * std::tanh((magnitude - LIMITER_KNEE) / headroom);
        samples[i] = std::copysign(limited, x);
      } else {
        samples[i] = x;
      }
    }
  }

  jitter_buffer_t::jitter_buffer_t(std::size_t target_samples, std::size_t max_samples):
      ring(std::max<std::size_t>(max_samples, 1)),
      target(std::min(target_samples, ring.size())) {
  }

  void jitter_buffer_t::push(const float *samples, std::size_t count) {
    std::lock_guard lock {mutex};

    const auto capacity = ring.size();
    if (count > capacity) {
      dropped_samples += count - capacity;
      samples += count - capacity;
      count = capacity;
    }

    const auto overflow = size + count > capacity ? size + count - capacity : 0;
    if (overflow) {
      head = (head + overflow) % capacity;
      size -= overflow;
      dropped_samples += overflow;
    }

    auto tail = (head + size) % capacity;
    for (std::size_t i = 0; i < count; ++i) {
      ring[tail] = samples[i];
      tail = tail + 1 == capacity ? 0 : tail + 1;
    }
    size += count;
  }

  void jitter_buffer_t::pull(float *out, std::size_t count) {
    std::lock_guard lock {mutex};

    if (!primed && size >= target) {
      primed = true;
    }

    std::size_t copied = 0;
    if (primed) {
      const auto capacity = ring.size();
      copied = std::min(count, size);
      for (std::size_t i = 0; i < copied; ++i) {
        out[i] = ring[head];
        head = head + 1 == capacity ? 0 : head + 1;
      }
      size -= copied;
      if (size == 0) {
        primed = false;
      }
    }

    std::fill(out + copied, out + count, 0.0f);
  }

  std::size_t jitter_buffer_t::buffered() const {
    std::lock_guard lock {mutex};
    return size;
  }

  std::size_t jitter_buffer_t::dropped() const {
    std::lock_guard lock {mutex};
    return dropped_samples;
  }

  decoder_t::decoder_t() {
    int err = OPUS_OK;
    opus = opus_decoder_create(SAMPLE_RATE, CHANNELS, &err);
    if (err != OPUS_OK) {
      BOOST_LOG(error) << "Client microphone: couldn't create Opus decoder: "sv << opus_strerror(err);
      opus = nullptr;
    }
  }

  decoder_t::~decoder_t() {
    if (opus) {
      opus_decoder_destroy(opus);
    }
  }

  bool decoder_t::valid() const {
    return opus != nullptr;
  }

  bool decoder_t::decode(const packet_t &packet, std::vector<float> &out) {
    out.clear();
    if (!opus || packet.opus.empty()) {
      return false;
    }

    const auto *data = reinterpret_cast<const unsigned char *>(packet.opus.data());
    const auto length = static_cast<opus_int32>(packet.opus.size());

    if (next_sequence) {
      const auto gap = static_cast<std::int32_t>(packet.sequence - *next_sequence);
      if (gap < 0) {
        ++late_packets;
        return false;
      }

      if (gap > 0) {
        lost_frames += static_cast<std::uint32_t>(gap);
        if (static_cast<std::uint32_t>(gap) <= MAX_CONCEALED_FRAMES) {
          out.resize(static_cast<std::size_t>(gap) * frame_samples);
          for (std::int32_t i = 0; i < gap; ++i) {
            // The frame right before this packet can be rebuilt from its in-band FEC; older ones use PLC.
            const bool fec = i == gap - 1;
            const auto decoded = opus_decode_float(opus, fec ? data : nullptr, fec ? length : 0, out.data() + i * frame_samples, frame_samples, fec ? 1 : 0);
            if (decoded != frame_samples) {
              std::fill_n(out.data() + i * frame_samples, frame_samples, 0.0f);
            }
          }
        } else {
          opus_decoder_ctl(opus, OPUS_RESET_STATE);
        }
      }
    }

    const auto offset = out.size();
    out.resize(offset + MAX_FRAME_SAMPLES * CHANNELS);
    const auto decoded = opus_decode_float(opus, data, length, out.data() + offset, MAX_FRAME_SAMPLES, 0);
    if (decoded < 0) {
      BOOST_LOG(warning) << "Client microphone: couldn't decode frame: "sv << opus_strerror(decoded);
      out.resize(offset);
      next_sequence = packet.sequence + 1;
      return offset > 0;
    }

    out.resize(offset + static_cast<std::size_t>(decoded) * CHANNELS);
    frame_samples = decoded;
    next_sequence = packet.sequence + 1;
    return true;
  }

  std::uint64_t decoder_t::lost() const {
    return lost_frames;
  }

  std::uint64_t decoder_t::late() const {
    return late_packets;
  }

  receiver_t::receiver_t(std::unique_ptr<platf::client_mic_sink_t> sink):
      sink {std::move(sink)} {
    if (config::audio.client_mic_echo_cancellation) {
      echo_canceller = std::make_unique<echo_canceller_t>(stream_reference());
    }
  }

  receiver_t::~receiver_t() {
    if (packets) {
      BOOST_LOG(info) << "Client microphone stopped: "sv << packets << " frames, "sv << decoder.lost() << " lost, "sv << decoder.late() << " late"sv;
    }
  }

  void receiver_t::receive(std::string_view payload) {
    const auto packet = parse_packet(payload);
    if (!packet) {
      BOOST_LOG(warning) << "Client microphone: runt packet"sv;
      return;
    }

    if (packets++ == 0) {
      BOOST_LOG(info) << "Client microphone active"sv;
    }

    if (decoder.decode(*packet, samples)) {
      if (echo_canceller) {
        echo_canceller->process(samples.data(), samples.size());
      }
      if (sink) {
        sink->write(samples.data(), samples.size());
      }
    }
  }

  std::unique_ptr<receiver_t> alloc() {
    if (!config::audio.client_mic) {
      BOOST_LOG(info) << "Client microphone received but client_mic is disabled"sv;
      return nullptr;
    }

    auto sink = platf::client_mic_sink(SAMPLE_RATE, CHANNELS);
    if (!sink) {
      BOOST_LOG(warning) << "Client microphone received but no virtual microphone is available"sv;
      return nullptr;
    }

    return std::make_unique<receiver_t>(std::move(sink));
  }
}  // namespace client_mic

#ifndef __APPLE__
namespace platf {
  std::unique_ptr<client_mic_sink_t> client_mic_sink(int, int) {
    return nullptr;
  }
}  // namespace platf
#endif
