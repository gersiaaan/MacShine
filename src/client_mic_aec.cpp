/**
 * @file src/client_mic_aec.cpp
 * @brief Definitions for removing the stream's own sound from the client's microphone (echo cancellation).
 */
// standard includes
#include <algorithm>
#include <cmath>
#include <numbers>

// local includes
#include "client_mic_aec.h"
#include "logging.h"

using namespace std::literals;

namespace client_mic {
  namespace {
    constexpr float STEP = 0.5f;  ///< NLMS step size at full adaptation speed.
    constexpr float POWER_FLOOR = 1e-6f;  ///< Keeps the normalisation finite in near-silence.
    constexpr float SILENT_REFERENCE = 1e-7f;  ///< Mean square below which the reference is treated as silent.
    constexpr std::uint64_t MAX_LAG = 28800;  ///< Reference lead (0.6 s) beyond which the cursor is re-aligned.
    constexpr std::uint64_t ALIGN_MARGIN = 3840;  ///< Cursor placed this far (80 ms) behind the newest reference, to absorb arrival jitter.
  }  // namespace

  void fft(std::vector<std::complex<float>> &data, bool inverse) {
    const auto n = data.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
      std::size_t bit = n >> 1;
      for (; j & bit; bit >>= 1) {
        j ^= bit;
      }
      j ^= bit;
      if (i < j) {
        std::swap(data[i], data[j]);
      }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
      const float angle = 2.0f * std::numbers::pi_v<float> / static_cast<float>(len) * (inverse ? 1.0f : -1.0f);
      const std::complex<float> step {std::cos(angle), std::sin(angle)};
      for (std::size_t i = 0; i < n; i += len) {
        std::complex<float> w {1.0f, 0.0f};
        for (std::size_t k = 0; k < len / 2; ++k) {
          const auto u = data[i + k];
          const auto v = data[i + k + len / 2] * w;
          data[i + k] = u + v;
          data[i + k + len / 2] = u - v;
          w *= step;
        }
      }
    }
    if (inverse) {
      const float scale = 1.0f / static_cast<float>(n);
      for (auto &x : data) {
        x *= scale;
      }
    }
  }

  reference_buffer_t::reference_buffer_t():
      ring(AEC_REFERENCE_CAPACITY) {
  }

  void reference_buffer_t::push(const float *samples, std::size_t count, int channels) {
    if (channels <= 0) {
      return;
    }
    std::lock_guard lock {mutex};
    const auto frames = count / static_cast<std::size_t>(channels);
    for (std::size_t f = 0; f < frames; ++f) {
      float sum = 0;
      for (int c = 0; c < channels; ++c) {
        sum += samples[f * channels + c];
      }
      ring[written % ring.size()] = sum / static_cast<float>(channels);
      ++written;
    }
  }

  std::uint64_t reference_buffer_t::write_position() const {
    std::lock_guard lock {mutex};
    return written;
  }

  void reference_buffer_t::read(std::uint64_t position, float *out, std::size_t count) const {
    std::lock_guard lock {mutex};
    const auto oldest = written > ring.size() ? written - ring.size() : 0;
    for (std::size_t i = 0; i < count; ++i) {
      const auto p = position + i;
      out[i] = (p >= oldest && p < written) ? ring[p % ring.size()] : 0.0f;
    }
  }

  void reference_buffer_t::clear() {
    std::lock_guard lock {mutex};
    std::fill(ring.begin(), ring.end(), 0.0f);
    written = 0;
  }

  reference_buffer_t &stream_reference() {
    static reference_buffer_t reference;
    return reference;
  }

  echo_canceller_t::echo_canceller_t(const reference_buffer_t &reference):
      reference {reference},
      previous_ref(AEC_BLOCK),
      history(AEC_PARTITIONS, std::vector<std::complex<float>>(AEC_FFT)),
      weights(AEC_PARTITIONS, std::vector<std::complex<float>>(AEC_FFT)),
      power(AEC_FFT),
      scratch(AEC_FFT) {
  }

  void echo_canceller_t::resync() {
    if (synced) {
      ++resync_count;
      BOOST_LOG(info) << "Client microphone: echo canceller re-aligned to the stream"sv;
    }
    synced = true;
    const auto newest = reference.write_position();
    cursor = newest > ALIGN_MARGIN + AEC_BLOCK ? newest - ALIGN_MARGIN - AEC_BLOCK : 0;
    std::fill(previous_ref.begin(), previous_ref.end(), 0.0f);
    for (auto &h : history) {
      std::fill(h.begin(), h.end(), std::complex<float> {});
    }
    for (auto &w : weights) {
      std::fill(w.begin(), w.end(), std::complex<float> {});
    }
    std::fill(power.begin(), power.end(), 0.0f);
  }

  void echo_canceller_t::process(float *samples, std::size_t count) {
    in_queue.insert(in_queue.end(), samples, samples + count);
    while (in_queue.size() >= AEC_BLOCK) {
      process_block(in_queue.data());
      out_queue.insert(out_queue.end(), in_queue.begin(), in_queue.begin() + AEC_BLOCK);
      in_queue.erase(in_queue.begin(), in_queue.begin() + AEC_BLOCK);
    }

    // Return exactly as many samples as came in; the first call pads with silence for the block lag.
    if (out_queue.size() < count) {
      out_queue.insert(out_queue.begin(), count - out_queue.size(), 0.0f);
    }
    std::copy_n(out_queue.begin(), count, samples);
    out_queue.erase(out_queue.begin(), out_queue.begin() + count);
  }

  void echo_canceller_t::process_block(float *mic) {
    const auto newest = reference.write_position();
    if (!synced || cursor + AEC_BLOCK > newest || newest - cursor > MAX_LAG) {
      // The microphone ran ahead of, or fell far behind, the stream (session start, clock drift, stalls).
      if (synced && newest == resync_at) {
        return;  // No stream sound since the last re-alignment: nothing to cancel, keep the microphone as is.
      }
      resync();
      resync_at = newest;
      if (cursor + AEC_BLOCK > newest) {
        return;
      }
    }

    std::vector<float> ref(AEC_BLOCK);
    reference.read(cursor, ref.data(), AEC_BLOCK);
    cursor += AEC_BLOCK;

    float ref_energy = 0;
    for (const auto x : ref) {
      ref_energy += x * x;
    }

    // Newest reference spectrum over [previous block, current block] (overlap-save).
    std::rotate(history.rbegin(), history.rbegin() + 1, history.rend());
    auto &spectrum = history.front();
    for (std::size_t i = 0; i < AEC_BLOCK; ++i) {
      spectrum[i] = previous_ref[i];
      spectrum[i + AEC_BLOCK] = ref[i];
    }
    fft(spectrum, false);
    previous_ref = ref;

    // Reference power per bin over the whole history, recomputed so it never drifts.
    std::fill(power.begin(), power.end(), 0.0f);
    for (const auto &h : history) {
      for (std::size_t k = 0; k < AEC_FFT; ++k) {
        power[k] += std::norm(h[k]);
      }
    }

    // Echo estimate: sum of partitions times the delayed reference spectra; keep the last block.
    std::fill(scratch.begin(), scratch.end(), std::complex<float> {});
    for (std::size_t p = 0; p < AEC_PARTITIONS; ++p) {
      const auto &h = history[p];
      const auto &w = weights[p];
      for (std::size_t k = 0; k < AEC_FFT; ++k) {
        scratch[k] += w[k] * h[k];
      }
    }
    fft(scratch, true);

    std::vector<float> error(AEC_BLOCK);
    float mic_energy = 0;
    for (std::size_t i = 0; i < AEC_BLOCK; ++i) {
      error[i] = mic[i] - scratch[i + AEC_BLOCK].real();
      mic_energy += mic[i] * mic[i];
    }

    // Adapt only while the stream plays something. When the microphone is clearly louder than the stream
    // it could have picked up, the user is talking (double talk): learn very slowly so the voice is kept.
    // The echo from a speaker is quieter at the microphone than the stream itself.
    if (ref_energy / AEC_BLOCK > SILENT_REFERENCE) {
      const float step = mic_energy > 2.0f * ref_energy ? STEP * 0.05f : STEP;

      std::fill(scratch.begin(), scratch.end(), std::complex<float> {});
      for (std::size_t i = 0; i < AEC_BLOCK; ++i) {
        scratch[i + AEC_BLOCK] = error[i];
      }
      fft(scratch, false);
      for (std::size_t k = 0; k < AEC_FFT; ++k) {
        scratch[k] *= step / (power[k] + POWER_FLOOR);
      }
      for (std::size_t p = 0; p < AEC_PARTITIONS; ++p) {
        const auto &h = history[p];
        auto &w = weights[p];
        for (std::size_t k = 0; k < AEC_FFT; ++k) {
          w[k] += std::conj(h[k]) * scratch[k];
        }
      }

      // Gradient constraint, two partitions per block: keep each partition's impulse response causal.
      for (int n = 0; n < 2; ++n) {
        auto &w = weights[constrain_next];
        fft(w, true);
        std::fill(w.begin() + AEC_BLOCK, w.end(), std::complex<float> {});
        fft(w, false);
        constrain_next = (constrain_next + 1) % AEC_PARTITIONS;
      }
    }

    std::copy(error.begin(), error.end(), mic);
  }

  std::uint64_t echo_canceller_t::resyncs() const {
    return resync_count;
  }
}  // namespace client_mic
