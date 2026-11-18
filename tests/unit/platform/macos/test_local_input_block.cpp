/**
 * @file tests/unit/platform/macos/test_local_input_block.cpp
 * @brief Tests for ignoring the Mac's own keyboard and trackpad while streaming.
 */

#ifdef __APPLE__
  // test includes
  #include "../../../tests_common.h"

  // platform includes
  #include <ApplicationServices/ApplicationServices.h>
  #include <Carbon/Carbon.h>

  // local includes
  #include "src/platform/macos/local_input_block.h"

namespace {
  constexpr std::int64_t OWN_PID {4242};
  constexpr std::int64_t HARDWARE_PID {0};
  constexpr std::int64_t OTHER_APP_PID {777};
}  // namespace

TEST(MacosLocalInputBlockTest, PassesEverythingSunshinePosted) {
  using platf::local_input_block::should_block;
  EXPECT_FALSE(should_block(kCGEventKeyDown, kVK_ANSI_A, OWN_PID, OWN_PID));
  EXPECT_FALSE(should_block(kCGEventFlagsChanged, kVK_Shift, OWN_PID, OWN_PID));
  EXPECT_FALSE(should_block(kCGEventMouseMoved, 0, OWN_PID, OWN_PID));
  EXPECT_FALSE(should_block(kCGEventScrollWheel, 0, OWN_PID, OWN_PID));
  EXPECT_FALSE(should_block(kCGEventLeftMouseDown, 0, OWN_PID, OWN_PID));
}

TEST(MacosLocalInputBlockTest, BlocksLocalKeysExceptEscape) {
  using platf::local_input_block::should_block;
  EXPECT_TRUE(should_block(kCGEventKeyDown, kVK_ANSI_A, HARDWARE_PID, OWN_PID));
  EXPECT_TRUE(should_block(kCGEventKeyUp, kVK_Return, HARDWARE_PID, OWN_PID));
  EXPECT_FALSE(should_block(kCGEventKeyDown, kVK_Escape, HARDWARE_PID, OWN_PID));
  EXPECT_FALSE(should_block(kCGEventKeyUp, kVK_Escape, HARDWARE_PID, OWN_PID));
}

TEST(MacosLocalInputBlockTest, BlocksLocalModifiersExceptCommand) {
  using platf::local_input_block::should_block;
  EXPECT_FALSE(should_block(kCGEventFlagsChanged, kVK_Command, HARDWARE_PID, OWN_PID));
  EXPECT_FALSE(should_block(kCGEventFlagsChanged, kVK_RightCommand, HARDWARE_PID, OWN_PID));
  EXPECT_TRUE(should_block(kCGEventFlagsChanged, kVK_Shift, HARDWARE_PID, OWN_PID));
  EXPECT_TRUE(should_block(kCGEventFlagsChanged, kVK_Option, HARDWARE_PID, OWN_PID));
  EXPECT_TRUE(should_block(kCGEventFlagsChanged, kVK_Control, HARDWARE_PID, OWN_PID));
}

TEST(MacosLocalInputBlockTest, BlocksLocalPointerAndSystemEvents) {
  using platf::local_input_block::should_block;
  EXPECT_TRUE(should_block(kCGEventMouseMoved, 0, HARDWARE_PID, OWN_PID));
  EXPECT_TRUE(should_block(kCGEventLeftMouseDown, 0, HARDWARE_PID, OWN_PID));
  EXPECT_TRUE(should_block(kCGEventScrollWheel, 0, HARDWARE_PID, OWN_PID));
  EXPECT_TRUE(should_block(29, 0, HARDWARE_PID, OWN_PID));  // trackpad gesture
  EXPECT_TRUE(should_block(14, 0, HARDWARE_PID, OWN_PID));  // system-defined: brightness and media keys
}

TEST(MacosLocalInputBlockTest, BlocksInputPostedByOtherApps) {
  using platf::local_input_block::should_block;
  EXPECT_TRUE(should_block(kCGEventKeyDown, kVK_ANSI_A, OTHER_APP_PID, OWN_PID));
  EXPECT_TRUE(should_block(kCGEventMouseMoved, 0, OTHER_APP_PID, OWN_PID));
  EXPECT_FALSE(should_block(kCGEventKeyDown, kVK_Escape, OTHER_APP_PID, OWN_PID));
}

TEST(MacosLocalInputBlockTest, StopWithoutStartIsSafe) {
  platf::local_input_block::stop();
  platf::local_input_block::stop();
  EXPECT_FALSE(platf::local_input_block::active());
}

TEST(MacosLocalInputBlockTest, StartAndStopRestoreInput) {
  // Without the Accessibility permission the tap cannot be installed and start() reports that.
  const bool started {platf::local_input_block::start()};
  EXPECT_EQ(started, platf::local_input_block::active());
  EXPECT_EQ(started, platf::local_input_block::start());
  platf::local_input_block::stop();
  EXPECT_FALSE(platf::local_input_block::active());
}
#endif
