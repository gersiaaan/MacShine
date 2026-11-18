/**
 * @file src/client_mic_aec.h
 * @brief Declarations for removing the stream's own sound from the client's microphone (echo cancellation).
 */
#pragma once

// standard includes
#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace client_mic {
  constexpr std::size_t AEC_BLOCK = 512;  ///< Samples processed per filter step (about 10.7 ms at 48 kHz).
  constexpr std::size_t AEC_FFT = 2 * AEC_BLOCK;  ///< FFT size of the overlap-save filter.
  constexpr std::size_t AEC_PARTITIONS = 48;  ///< Filter partitions; echo paths up to 48 blocks (about 512 ms) long are modelled.
  constexpr std::size_t AEC_REFERENCE_CAPACITY = 96000;  ///< Reference history kept, in samples (2 s at 48 kHz).

  /**
   * @brief In-place radix-2 FFT.
   *
   * @param data Complex samples; the size must be a power of two.
   * @param inverse Compute the inverse transform (scaled by 1/N).
   */
  void fft(std::vector<std::complex<float>> &data, bool inverse);

  /**
   * @brief History of the sound sent to the client, as mono samples on one running timeline.
   */
  class reference_buffer_t {
  public:
    reference_buffer_t();

    /**
     * @brief Add sound that was just sent to the client.
     *
     * @param samples Interleaved samples.
     * @param count Number of samples (frames times channels).
     * @param channels Channel count; the channels are averaged to mono.
     */
    void push(const float *samples, std::size_t count, int channels);

    /**
     * @brief Total number of mono samples pushed so far: the position just after the newest sample.
     *
     * @return Write position.
     */
    std::uint64_t write_position() const;

    /**
     * @brief Copy samples starting at a timeline position.
     *
     * Positions that are not (or no longer) in the history read as silence.
     *
     * @param position Timeline position of the first sample.
     * @param out Destination for @p count samples.
     * @param count Number of samples.
     */
    void read(std::uint64_t position, float *out, std::size_t count) const;

    /**
     * @brief Forget all history (used between sessions and in tests).
     */
    void clear();

  private:
    mutable std::mutex mutex;  ///< Guards the ring and the position.
    std::vector<float> ring;  ///< Last #AEC_REFERENCE_CAPACITY samples.
    std::uint64_t written {};  ///< Total samples pushed.
  };

  /**
   * @brief The process-wide reference: everything the audio thread streams to the client.
   *
   * @return Shared reference buffer.
   */
  reference_buffer_t &stream_reference();

  /**
   * @brief Removes the stream's sound that the client's microphone picked up from its speaker.
   *
   * A partitioned-block frequency-domain NLMS filter (overlap-save) learns the path from the sound sent to
   * the client to what comes back in the microphone, including the network and playback delay, and
   * subtracts its estimate. Adaptation slows down while the client's user talks, so their voice is kept.
   */
  class echo_canceller_t {
  public:
    /**
     * @brief Create a canceller reading the reference from @p reference.
     *
     * @param reference Sound sent to the client.
     */
    explicit echo_canceller_t(const reference_buffer_t &reference);

    /**
     * @brief Remove the echo from microphone samples in place.
     *
     * Output lags the input by up to one block; the samples returned are always as many as passed in.
     *
     * @param samples Mono 48 kHz microphone samples.
     * @param count Number of samples.
     */
    void process(float *samples, std::size_t count);

    /**
     * @brief Times the reference timeline had to be re-aligned with the microphone.
     *
     * @return Resync count.
     */
    std::uint64_t resyncs() const;

  private:
    /**
     * @brief Run the filter on one block of #AEC_BLOCK microphone samples.
     *
     * @param mic Microphone block in, echo-free block out.
     */
    void process_block(float *mic);

    /**
     * @brief Point the reference cursor at the newest sound and start learning from scratch.
     */
    void resync();

    const reference_buffer_t &reference;  ///< Sound sent to the client.
    bool synced {};  ///< Whether the reference cursor has been placed.
    std::uint64_t cursor {};  ///< Reference position matching the next microphone block.
    std::uint64_t resync_count {};  ///< Times resync() ran after the first placement.
    std::uint64_t resync_at {};  ///< Reference write position at the last resync().
    std::vector<float> previous_ref;  ///< Reference block before the current one (overlap-save input).
    std::vector<std::vector<std::complex<float>>> history;  ///< Reference spectra, newest first.
    std::vector<std::vector<std::complex<float>>> weights;  ///< Filter partitions in the frequency domain.
    std::vector<float> power;  ///< Per-bin reference power summed over the history.
    std::size_t constrain_next {};  ///< Next partition whose weights are made causal.
    std::vector<float> in_queue;  ///< Microphone samples waiting for a full block.
    std::vector<float> out_queue;  ///< Processed samples waiting to be returned.
    std::vector<std::complex<float>> scratch;  ///< Reused FFT buffer.
  };
}  // namespace client_mic
