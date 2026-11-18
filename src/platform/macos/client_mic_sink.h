/**
 * @file src/platform/macos/client_mic_sink.h
 * @brief Declarations for playing the client's microphone into the "MacShine" virtual input device.
 */
#pragma once

// standard includes
#include <optional>
#include <string>

// lib includes
#include <CoreAudio/CoreAudio.h>

namespace platf::client_mic_device {
  /**
   * @brief UID of the visible "MacShine" input device that apps record from.
   */
  inline const std::string INPUT_UID = "MacShine_UID";
  /**
   * @brief UID of the hidden "MacShine Sender" output device that MacShine plays into.
   */
  inline const std::string SENDER_UID = "MacShine_2_UID";

  /**
   * @brief Look up an audio device by its UID.
   *
   * @param uid Device UID.
   * @return Device ID, or nothing when no such device is installed.
   */
  std::optional<AudioObjectID> find_by_uid(const std::string &uid);

  /**
   * @brief Current default input device of the system.
   *
   * @return Device ID, or nothing when it can't be read.
   */
  std::optional<AudioObjectID> default_input();

  /**
   * @brief Change the default input device of the system.
   *
   * @param device Device to make the default input.
   * @return True on success.
   */
  bool set_default_input(AudioObjectID device);
}  // namespace platf::client_mic_device
