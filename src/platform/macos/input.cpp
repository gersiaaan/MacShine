/**
 * @file src/platform/macos/input.cpp
 * @brief Definitions for libvirtualhid-backed macOS input handling.
 */

// platform includes
#include <ApplicationServices/ApplicationServices.h>

// standard includes
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

// lib includes
#include <libvirtualhid/license.hpp>

// local includes
#include "src/config.h"
#include "src/platform/virtualhid_input.h"

namespace platf {

  std::optional<util::point_t> get_mouse_loc(input_t & /*input*/) {
    const auto event = CGEventCreate(nullptr);
    if (!event) {
      return std::nullopt;
    }

    const auto current = CGEventGetLocation(event);
    CFRelease(event);
    return util::point_t {current.x, current.y};
  }

  platform_caps::caps_t get_capabilities() {
    platform_caps::caps_t caps = 0;
    // macOS has no touch input, but touches and pens are turned into mouse gestures (see touch_input.h).
    if (config::input.native_pen_touch) {
      caps |= platform_caps::pen_touch;
    }
    if (config::audio.client_mic) {
      caps |= platform_caps::client_mic;
    }
    const auto runtime = virtualhid::create_runtime();
    if (!runtime) {
      return caps;
    }

    const auto &capabilities = runtime->capabilities();
    if (config::input.gamepad_driver != config::GAMEPAD_DRIVER_NONE && capabilities.supports_gamepad && lvh::get_license_status().license.licensed() && virtualhid::configured_gamepad_supports_controller_extensions()) {
      caps |= platform_caps::controller_touch;
    }
    return caps;
  }

  std::vector<supported_gamepad_t> &supported_gamepads(input_t *input) {
    static std::vector<supported_gamepad_t> gamepads;
    if (!input || !input->get()) {
      gamepads = virtualhid::static_supported_gamepads();
      return gamepads;
    }

    if (config::input.gamepad_driver == config::GAMEPAD_DRIVER_NONE) {
      gamepads.clear();
      return gamepads;
    }

    const auto licensed = lvh::get_license_status().license.licensed();
    gamepads = virtualhid::supported_gamepads(virtualhid::get_input_context(*input).runtime.get(), false, licensed, true);
    return gamepads;
  }

}  // namespace platf
