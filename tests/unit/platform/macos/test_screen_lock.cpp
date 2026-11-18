/**
 * @file tests/unit/platform/macos/test_screen_lock.cpp
 * @brief Test platf::screen_locked() on macOS.
 */

// Only compile these tests on macOS
#ifdef __APPLE__

  // test includes
  #include "../../../tests_common.h"

  // lib includes
  #include <CoreGraphics/CoreGraphics.h>

  // local includes
  #include <src/platform/common.h>

TEST(MacosScreenLockTest, MatchesTheSessionDictionary) {
  CFDictionaryRef session = CGSessionCopyCurrentDictionary();
  bool expected = true;
  if (session) {
    const auto value = CFDictionaryGetValue(session, CFSTR("CGSSessionScreenIsLocked"));
    expected = value && CFBooleanGetValue(static_cast<CFBooleanRef>(value));
    CFRelease(session);
  }
  EXPECT_EQ(platf::screen_locked(), expected);
}

#endif
