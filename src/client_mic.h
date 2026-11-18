/**
 * @file src/client_mic.h
 * @brief Declarations for receiving the client's microphone and playing it into the host's virtual microphone.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

// local includes
#include "client_mic_aec.h"

/**
 * @brief Opaque libopus decoder state.
 */
struct OpusDecoder;

namespace platf {
  class client_mic_sink_t;
}  // namespace platf

/**
 * @brief Client microphone upstream (a MacShine protocol extension).
 *
 * The client encodes its microphone as mono 48 kHz Opus and sends each frame on the encrypted control
 * stream as packet type #PACKET_TYPE. The payload is a little-endian 32-bit sequence number followed by
 * one Opus packet. A payload holding only the sequence number means the client turned its microphone off.
 * The host decodes the audio and plays it into a virtual input device that apps record from.
 */
namespace client_mic {
  constexpr std::uint16_t PACKET_TYPE = 0x5510;  ///< Control-stream packet type carrying one microphone frame.
  constexpr int SAMPLE_RATE = 48000;  ///< Sample rate of the microphone stream.
  constexpr int CHANNELS = 1;  ///< Channel count of the microphone stream.
  constexpr int DEFAULT_FRAME_SAMPLES = 960;  ///< Samples in a 20 ms frame, assumed until the first packet decodes.
  constexpr int MAX_FRAME_SAMPLES = 5760;  ///< Samples in the longest Opus frame (120 ms).
  constexpr std::uint32_t MAX_CONCEALED_FRAMES = 5;  ///< Longest gap that is filled by concealment instead of a decoder reset.
  constexpr std::size_t JITTER_TARGET_SAMPLES = 1920;  ///< Audio queued before playback starts (40 ms).
  constexpr std::size_t JITTER_MAX_SAMPLES = 9600;  ///< Queued audio beyond this is dropped to bound latency (200 ms).

  /**
   * @brief One parsed microphone packet.
   */
  struct packet_t {
    std::uint32_t sequence;  ///< Sequence number assigned by the client, one per frame.
    std::string_view opus;  ///< Opus packet, pointing into the received payload.
  };

  /**
   * @brief Split a control-stream payload into its sequence number and Opus packet.
   *
   * @param payload Payload of a #PACKET_TYPE message.
   * @return Parsed packet, or nothing when the payload is too short to hold audio.
   */
  std::optional<packet_t> parse_packet(std::string_view payload);

  /**
   * @brief Whether a payload tells the host that the client turned its microphone off.
   *
   * @param payload Payload of a #PACKET_TYPE message.
   * @return True for a payload that holds only a sequence number.
   */
  bool is_stop_packet(std::string_view payload);

  constexpr float LIMITER_KNEE = 0.7f;  ///< Level above which apply_level() starts compressing peaks.

  /**
   * @brief Change the level of samples in place and soften peaks so they never clip.
   *
   * Samples are scaled by @p gain_db; anything above #LIMITER_KNEE is compressed smoothly towards,
   * but never reaching, full scale.
   *
   * @param samples Samples to change.
   * @param count Number of samples.
   * @param gain_db Level change in dB; 0 leaves the level as it is.
   */
  void apply_level(float *samples, std::size_t count, float gain_db);

  /**
   * @brief Bounded FIFO between the network thread and the audio device that absorbs network jitter.
   *
   * Playback waits until the target amount is queued, plays silence after running dry until the target is
   * reached again, and drops the oldest audio when more than the maximum is queued.
   */
  class jitter_buffer_t {
  public:
    /**
     * @brief Create an empty buffer.
     *
     * @param target_samples Samples queued before playback (re)starts.
     * @param max_samples Most samples kept; older ones are dropped beyond this.
     */
    jitter_buffer_t(std::size_t target_samples, std::size_t max_samples);

    /**
     * @brief Queue decoded samples.
     *
     * @param samples Samples to queue.
     * @param count Number of samples.
     */
    void push(const float *samples, std::size_t count);

    /**
     * @brief Take samples for playback, filling with silence while the buffer is not primed.
     *
     * @param out Destination for exactly @p count samples.
     * @param count Number of samples requested.
     */
    void pull(float *out, std::size_t count);

    /**
     * @brief Number of samples currently queued.
     *
     * @return Queued sample count.
     */
    std::size_t buffered() const;

    /**
     * @brief Number of samples dropped so far because the buffer was full.
     *
     * @return Dropped sample count.
     */
    std::size_t dropped() const;

  private:
    mutable std::mutex mutex;  ///< Guards all fields; held only for short copies.
    std::vector<float> ring;  ///< Ring storage with capacity for max_samples.
    std::size_t head {};  ///< Index of the oldest queued sample.
    std::size_t size {};  ///< Number of queued samples.
    std::size_t target;  ///< Samples needed before playback (re)starts.
    std::size_t dropped_samples {};  ///< Samples dropped because the buffer was full.
    bool primed {};  ///< Whether playback is currently consuming queued audio.
  };

  /**
   * @brief Opus decoder that conceals lost or reordered frames using the sequence numbers.
   */
  class decoder_t {
  public:
    decoder_t();
    ~decoder_t();
    decoder_t(const decoder_t &) = delete;
    decoder_t &operator=(const decoder_t &) = delete;

    /**
     * @brief Whether the Opus decoder could be created.
     *
     * @return True when decode() can be used.
     */
    bool valid() const;

    /**
     * @brief Decode one packet, concealing any frames missing before it.
     *
     * @param packet Packet to decode.
     * @param out Receives the decoded samples, concealed ones first; cleared before use.
     * @return True when samples were produced, false when the packet was late, duplicated or invalid.
     */
    bool decode(const packet_t &packet, std::vector<float> &out);

    /**
     * @brief Number of frames that never arrived.
     *
     * @return Lost frame count.
     */
    std::uint64_t lost() const;

    /**
     * @brief Number of packets dropped because they arrived after a newer one.
     *
     * @return Late packet count.
     */
    std::uint64_t late() const;

  private:
    OpusDecoder *opus {};  ///< Decoder state, owned.
    std::optional<std::uint32_t> next_sequence;  ///< Sequence number expected next.
    int frame_samples {DEFAULT_FRAME_SAMPLES};  ///< Duration of the last decoded frame, used for concealment.
    std::uint64_t lost_frames {};  ///< Frames that never arrived.
    std::uint64_t late_packets {};  ///< Packets that arrived after a newer one.
  };

  /**
   * @brief Receives microphone packets for one session and plays them into a sink.
   */
  class receiver_t {
  public:
    /**
     * @brief Create a receiver writing into @p sink.
     *
     * @param sink Destination of the decoded audio.
     */
    explicit receiver_t(std::unique_ptr<platf::client_mic_sink_t> sink);
    ~receiver_t();

    /**
     * @brief Handle one #PACKET_TYPE payload.
     *
     * @param payload Payload received on the control stream.
     */
    void receive(std::string_view payload);

  private:
    std::unique_ptr<platf::client_mic_sink_t> sink;  ///< Destination of the decoded audio.
    decoder_t decoder;  ///< Opus decoder.
    std::unique_ptr<echo_canceller_t> echo_canceller;  ///< Removes the stream's sound, or null when turned off.
    std::vector<float> samples;  ///< Reused decode buffer.
    std::uint64_t packets {};  ///< Packets received.
  };

  /**
   * @brief Create a receiver for a new session.
   *
   * @return Receiver, or nullptr when the feature is disabled or no virtual microphone is available.
   */
  std::unique_ptr<receiver_t> alloc();
}  // namespace client_mic
