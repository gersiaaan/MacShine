/**
 * @file src/platform/macos/client_mic_sink.cpp
 * @brief Definitions for playing the client's microphone into the "MacShine" virtual input device.
 */
// standard includes
#include <iterator>
#include <memory>

// lib includes
#include <AudioToolbox/AudioToolbox.h>

// local includes
#include "client_mic_sink.h"
#include "coreaudio_helpers.h"
#include "src/client_mic.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"

using namespace std::literals;

namespace platf {
  namespace client_mic_device {
    std::optional<AudioObjectID> find_by_uid(const std::string &uid) {
      CFStringRef cf_uid = CFStringCreateWithCString(kCFAllocatorDefault, uid.c_str(), kCFStringEncodingUTF8);
      if (!cf_uid) {
        return std::nullopt;
      }

      AudioObjectPropertyAddress address {
        kAudioHardwarePropertyTranslateUIDToDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
      };
      AudioObjectID device = kAudioObjectUnknown;
      UInt32 size = sizeof(device);
      const auto status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, sizeof(cf_uid), &cf_uid, &size, &device);
      CFRelease(cf_uid);

      if (status != noErr || device == kAudioObjectUnknown) {
        return std::nullopt;
      }
      return device;
    }

    std::optional<AudioObjectID> default_input() {
      AudioObjectPropertyAddress address {
        kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
      };
      AudioObjectID device = kAudioObjectUnknown;
      UInt32 size = sizeof(device);
      if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &device) != noErr || device == kAudioObjectUnknown) {
        return std::nullopt;
      }
      return device;
    }

    bool set_default_input(AudioObjectID device) {
      AudioObjectPropertyAddress address {
        kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
      };
      const auto status = AudioObjectSetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, sizeof(device), &device);
      if (status != noErr) {
        BOOST_LOG(warning) << "Client microphone: couldn't change the default input: "sv << ca::Status(status);
        return false;
      }
      return true;
    }
  }  // namespace client_mic_device

  /**
   * @brief Plays client microphone audio into the hidden "MacShine Sender" device through a HAL output unit.
   */
  class macos_client_mic_sink_t: public client_mic_sink_t {
  public:
    /**
     * @brief Create an unopened sink.
     *
     * @param channels Channel count of the written samples.
     */
    explicit macos_client_mic_sink_t(int channels):
        channels {channels},
        buffer {client_mic::JITTER_TARGET_SAMPLES * channels, client_mic::JITTER_MAX_SAMPLES * channels} {
    }

    ~macos_client_mic_sink_t() override {
      if (unit) {
        AudioOutputUnitStop(unit);
        AudioUnitUninitialize(unit);
        AudioComponentInstanceDispose(unit);
      }
      for (auto effect : {eq, isolation}) {
        if (effect) {
          AudioUnitUninitialize(effect);
          AudioComponentInstanceDispose(effect);
        }
      }

      // Give the input back, unless the user picked another one in the meantime.
      if (previous_input && input_device && client_mic_device::default_input() == input_device) {
        client_mic_device::set_default_input(*previous_input);
      }
    }

    /**
     * @brief Open and start the output unit on the sender device.
     *
     * @param sample_rate Sample rate of the written samples.
     * @return True when audio is flowing to the device.
     */
    bool open(int sample_rate) {
      const auto sender = client_mic_device::find_by_uid(client_mic_device::SENDER_UID);
      if (!sender) {
        BOOST_LOG(warning) << "Client microphone: the MacShine audio driver is not installed (scripts/macshine-mic)"sv;
        return false;
      }

      AudioComponentDescription description {};
      description.componentType = kAudioUnitType_Output;
      description.componentSubType = kAudioUnitSubType_HALOutput;
      description.componentManufacturer = kAudioUnitManufacturer_Apple;
      const auto component = AudioComponentFindNext(nullptr, &description);
      if (!component || AudioComponentInstanceNew(component, &unit) != noErr) {
        BOOST_LOG(error) << "Client microphone: couldn't create the output unit"sv;
        unit = nullptr;
        return false;
      }

      AudioObjectID device = *sender;
      AudioStreamBasicDescription format {};
      format.mSampleRate = sample_rate;
      format.mFormatID = kAudioFormatLinearPCM;
      format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
      format.mChannelsPerFrame = channels;
      format.mBitsPerChannel = 32;
      format.mBytesPerFrame = sizeof(float);
      format.mFramesPerPacket = 1;
      format.mBytesPerPacket = format.mBytesPerFrame;

      gain_db = static_cast<float>(config::audio.client_mic_gain_db);
      if (config::audio.client_mic_voice_isolation) {
        open_isolation(format);
      }
      if (config::audio.client_mic_eq) {
        open_eq(format);
      }

      AURenderCallbackStruct callback {&macos_client_mic_sink_t::render, this};

      OSStatus status = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device, sizeof(device));
      if (status == noErr) {
        status = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(format));
      }
      if (status == noErr) {
        status = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(callback));
      }
      if (status == noErr) {
        status = AudioUnitInitialize(unit);
      }
      if (status == noErr) {
        status = AudioOutputUnitStart(unit);
      }
      if (status != noErr) {
        BOOST_LOG(error) << "Client microphone: couldn't start playback into MacShine Sender: "sv << ca::Status(status);
        return false;
      }

      if (config::audio.client_mic_default_input) {
        input_device = client_mic_device::find_by_uid(client_mic_device::INPUT_UID);
        previous_input = client_mic_device::default_input();
        if (input_device && previous_input != input_device && client_mic_device::set_default_input(*input_device)) {
          BOOST_LOG(info) << "Client microphone: MacShine is now the default input"sv;
        } else {
          previous_input.reset();
        }
      }

      return true;
    }

    /**
     * @brief Set up Apple's voice isolation (AUSoundIsolation) in front of the output unit.
     *
     * Without it, or when it can't be created, the audio goes to the device unfiltered.
     *
     * @param format Stream format used on both sides of the effect.
     */
    void open_isolation(const AudioStreamBasicDescription &format) {
      AudioComponentDescription description {};
      description.componentType = kAudioUnitType_Effect;
      description.componentSubType = kAudioUnitSubType_AUSoundIsolation;
      description.componentManufacturer = kAudioUnitManufacturer_Apple;
      const auto component = AudioComponentFindNext(nullptr, &description);
      if (!component || AudioComponentInstanceNew(component, &isolation) != noErr) {
        BOOST_LOG(warning) << "Client microphone: voice isolation is not available on this Mac"sv;
        isolation = nullptr;
        return;
      }

      UInt32 max_frames = 4096;
      AURenderCallbackStruct input {&macos_client_mic_sink_t::pull, this};
      OSStatus status = AudioUnitSetProperty(isolation, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(format));
      if (status == noErr) {
        status = AudioUnitSetProperty(isolation, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, sizeof(format));
      }
      if (status == noErr) {
        status = AudioUnitSetProperty(isolation, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &max_frames, sizeof(max_frames));
      }
      if (status == noErr) {
        status = AudioUnitSetProperty(isolation, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &input, sizeof(input));
      }
      if (status == noErr) {
        status = AudioUnitInitialize(isolation);
      }
      if (status != noErr) {
        BOOST_LOG(warning) << "Client microphone: couldn't set up voice isolation: "sv << ca::Status(status);
        AudioComponentInstanceDispose(isolation);
        isolation = nullptr;
        return;
      }

      // The high quality model exists from macOS 15; older systems keep the standard one.
      AudioUnitSetParameter(isolation, kAUSoundIsolationParam_SoundToIsolate, kAudioUnitScope_Global, 0, kAUSoundIsolationSoundType_HighQualityVoice, 0);
      AudioUnitSetParameter(isolation, kAUSoundIsolationParam_WetDryMixPercent, kAudioUnitScope_Global, 0, 100, 0);
      BOOST_LOG(info) << "Client microphone: voice isolation on"sv;
    }

    /**
     * @brief Set up a three-band equalizer after the voice isolation that moves the client's microphone
     * towards an iPhone's voice tone: warmer lows, softer presence and sibilance.
     *
     * @param format Stream format used on both sides of the effect.
     */
    void open_eq(const AudioStreamBasicDescription &format) {
      AudioComponentDescription description {};
      description.componentType = kAudioUnitType_Effect;
      description.componentSubType = kAudioUnitSubType_NBandEQ;
      description.componentManufacturer = kAudioUnitManufacturer_Apple;
      const auto component = AudioComponentFindNext(nullptr, &description);
      if (!component || AudioComponentInstanceNew(component, &eq) != noErr) {
        eq = nullptr;
        return;
      }

      struct band_t {
        AudioUnitParameterValue type;  ///< Filter type.
        AudioUnitParameterValue frequency;  ///< Centre or corner frequency in Hz.
        AudioUnitParameterValue gain;  ///< Gain in dB.
        AudioUnitParameterValue bandwidth;  ///< Bandwidth in octaves (parametric bands).
      };

      // Measured against an iPhone 15 Pro Max recording of the same voice: the Duo lacks 4-6 dB below
      // 400 Hz and has 4-8 dB more at 1.6 kHz and 6 kHz.
      const band_t bands[] = {
        {kAUNBandEQFilterType_LowShelf, 250, 5, 1},
        {kAUNBandEQFilterType_Parametric, 1600, -4, 1.5f},
        {kAUNBandEQFilterType_Parametric, 6400, -6, 1},
      };
      UInt32 band_count = std::size(bands);
      UInt32 max_frames = 4096;
      AURenderCallbackStruct input {&macos_client_mic_sink_t::eq_input, this};
      OSStatus status = AudioUnitSetProperty(eq, kAUNBandEQProperty_NumberOfBands, kAudioUnitScope_Global, 0, &band_count, sizeof(band_count));
      if (status == noErr) {
        status = AudioUnitSetProperty(eq, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(format));
      }
      if (status == noErr) {
        status = AudioUnitSetProperty(eq, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, sizeof(format));
      }
      if (status == noErr) {
        status = AudioUnitSetProperty(eq, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &max_frames, sizeof(max_frames));
      }
      if (status == noErr) {
        status = AudioUnitSetProperty(eq, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &input, sizeof(input));
      }
      if (status == noErr) {
        status = AudioUnitInitialize(eq);
      }
      if (status != noErr) {
        BOOST_LOG(warning) << "Client microphone: couldn't set up the equalizer: "sv << ca::Status(status);
        AudioComponentInstanceDispose(eq);
        eq = nullptr;
        return;
      }

      for (UInt32 i = 0; i < band_count; ++i) {
        AudioUnitSetParameter(eq, kAUNBandEQParam_FilterType + i, kAudioUnitScope_Global, 0, bands[i].type, 0);
        AudioUnitSetParameter(eq, kAUNBandEQParam_Frequency + i, kAudioUnitScope_Global, 0, bands[i].frequency, 0);
        AudioUnitSetParameter(eq, kAUNBandEQParam_Gain + i, kAudioUnitScope_Global, 0, bands[i].gain, 0);
        AudioUnitSetParameter(eq, kAUNBandEQParam_Bandwidth + i, kAudioUnitScope_Global, 0, bands[i].bandwidth, 0);
        AudioUnitSetParameter(eq, kAUNBandEQParam_BypassBand + i, kAudioUnitScope_Global, 0, 0, 0);
      }
      BOOST_LOG(info) << "Client microphone: voice equalizer on"sv;
    }

    void write(const float *samples, std::size_t count) override {
      buffer.push(samples, count);
    }

  private:
    /**
     * @brief Render callback that feeds the output unit from the jitter buffer.
     *
     * @param ref_con The sink.
     * @param flags Render flags, unused.
     * @param timestamp Render time, unused.
     * @param bus Bus number, unused.
     * @param frames Number of frames requested.
     * @param data Buffers to fill.
     * @return Always noErr.
     */
    static OSStatus render(void *ref_con, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *timestamp, UInt32 bus, UInt32 frames, AudioBufferList *data) {
      auto *self = static_cast<macos_client_mic_sink_t *>(ref_con);
      if (!self->eq || AudioUnitRender(self->eq, flags, timestamp, 0, frames, data) != noErr) {
        eq_input(ref_con, flags, timestamp, bus, frames, data);
      }
      for (UInt32 i = 0; i < data->mNumberBuffers; ++i) {
        auto &out = data->mBuffers[i];
        client_mic::apply_level(static_cast<float *>(out.mData), out.mDataByteSize / sizeof(float), self->gain_db);
      }
      return noErr;
    }

    /**
     * @brief Input of the equalizer: the voice isolation's output, or the jitter buffer without it.
     *
     * @param ref_con The sink.
     * @param flags Render flags.
     * @param timestamp Render time.
     * @param bus Bus number.
     * @param frames Number of frames requested.
     * @param data Buffers to fill.
     * @return Always noErr.
     */
    static OSStatus eq_input(void *ref_con, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *timestamp, UInt32 bus, UInt32 frames, AudioBufferList *data) {
      auto *self = static_cast<macos_client_mic_sink_t *>(ref_con);
      if (!self->isolation || AudioUnitRender(self->isolation, flags, timestamp, 0, frames, data) != noErr) {
        pull(ref_con, flags, timestamp, bus, frames, data);
      }
      return noErr;
    }

    /**
     * @brief Fill buffers straight from the jitter buffer (input of the voice isolation, or the fallback path).
     *
     * @param ref_con The sink.
     * @param flags Render flags, unused.
     * @param timestamp Render time, unused.
     * @param bus Bus number, unused.
     * @param frames Number of frames requested.
     * @param data Buffers to fill.
     * @return Always noErr.
     */
    static OSStatus pull(void *ref_con, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *timestamp, UInt32 bus, UInt32 frames, AudioBufferList *data) {
      auto *self = static_cast<macos_client_mic_sink_t *>(ref_con);
      for (UInt32 i = 0; i < data->mNumberBuffers; ++i) {
        auto &out = data->mBuffers[i];
        self->buffer.pull(static_cast<float *>(out.mData), out.mDataByteSize / sizeof(float));
      }
      return noErr;
    }

    int channels;  ///< Channel count of the written samples.
    client_mic::jitter_buffer_t buffer;  ///< Audio waiting for the device.
    AudioComponentInstance unit {};  ///< HAL output unit on the sender device.
    AudioComponentInstance isolation {};  ///< Apple voice isolation effect, or null when off or unavailable.
    AudioComponentInstance eq {};  ///< Voice equalizer after the isolation, or null when off or unavailable.
    float gain_db {};  ///< Level change applied after the isolation, in dB.
    std::optional<AudioObjectID> input_device;  ///< The visible MacShine input, when it was made the default.
    std::optional<AudioObjectID> previous_input;  ///< Default input to restore when the sink closes.
  };

  std::unique_ptr<client_mic_sink_t> client_mic_sink(int sample_rate, int channels) {
    auto sink = std::make_unique<macos_client_mic_sink_t>(channels);
    if (!sink->open(sample_rate)) {
      return nullptr;
    }
    return sink;
  }
}  // namespace platf
