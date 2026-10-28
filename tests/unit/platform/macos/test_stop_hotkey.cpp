/**
 * @file tests/unit/platform/macos/test_stop_hotkey.cpp
 * @brief Tests for the macOS stop-stream hotkey.
 */

#ifdef __APPLE__
  // test includes
  #include "../../../tests_common.h"

  // local includes
  #include "src/platform/macos/stop_hotkey.h"

TEST(MacosStopHotkeyTest, UnregisterWithoutRegistrationIsSafe) {
  platf::stop_hotkey::unregister_hotkey();
  platf::stop_hotkey::unregister_hotkey();
  SUCCEED();
}
#endif
