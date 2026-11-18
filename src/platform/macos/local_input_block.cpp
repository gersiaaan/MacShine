/**
 * @file src/platform/macos/local_input_block.cpp
 * @brief Ignore the Mac's own keyboard and trackpad while a stream is active (MacShine).
 *
 * An event tap at the HID level sees every input event before the system acts on it. Events posted by
 * Sunshine carry its process id; local hardware events do not, so they can be dropped while the
 * client's input still goes through.
 */
// platform includes
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <unistd.h>

// standard includes
#include <atomic>
#include <future>
#include <mutex>
#include <thread>

// local includes
#include "local_input_block.h"
#include "src/logging.h"

using namespace std::literals;

namespace platf::local_input_block {
  namespace {
    std::mutex g_mutex;  ///< Guards the tap thread and its run loop.
    std::thread g_thread;  ///< Thread that runs the event tap.
    CFRunLoopRef g_run_loop {};  ///< Run loop of g_thread, stopped by stop().
    CFMachPortRef g_tap {};  ///< The event tap, re-enabled when the system disables it.
    std::atomic_bool g_active {false};  ///< Whether local input is blocked right now.
    std::atomic_bool g_stop {false};  ///< Asks the tap thread to exit.
    std::atomic_uint64_t g_blocked {0};  ///< Events dropped since start().
    std::atomic_bool g_logged_first {false};  ///< Whether the first dropped event was logged.

    /**
     * @brief Event tap callback: drop local events, keep the client's.
     *
     * @param type Event type.
     * @param event The event.
     * @return The event to pass on, or nullptr to drop it.
     */
    CGEventRef handle_event(CGEventTapProxy /*proxy*/, CGEventType type, CGEventRef event, void * /*user_data*/) {
      if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        if (g_tap) {
          CGEventTapEnable(g_tap, true);
        }
        return event;
      }

      const auto keycode {CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode)};
      const auto source_pid {CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID)};
      if (!should_block(type, keycode, source_pid, getpid())) {
        return event;
      }

      ++g_blocked;
      if (!g_logged_first.exchange(true)) {
        BOOST_LOG(info) << "Ignoring local input while streaming (first event: type "sv << type << ", source pid "sv << source_pid << ')';
      }
      return nullptr;
    }

    /**
     * @brief Body of the tap thread: install the tap and run until stop().
     *
     * @param ready Receives whether the tap could be installed.
     */
    void run_tap(std::promise<bool> ready) {
      // Every event type; the tap-disabled notifications arrive regardless of the mask.
      CFMachPortRef tap {CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault, kCGEventMaskForAllEvents, handle_event, nullptr)};
      if (!tap) {
        ready.set_value(false);
        return;
      }
      CFRunLoopSourceRef source {CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0)};
      {
        std::lock_guard lock {g_mutex};
        g_tap = tap;
        g_run_loop = CFRunLoopGetCurrent();
      }
      CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
      CGEventTapEnable(tap, true);
      ready.set_value(true);

      // Short slices, so a stop() that comes before the loop runs is still seen.
      while (!g_stop) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
      }

      CGEventTapEnable(tap, false);
      CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
      CFRelease(source);
      {
        std::lock_guard lock {g_mutex};
        g_tap = nullptr;
        g_run_loop = nullptr;
      }
      CFMachPortInvalidate(tap);
      CFRelease(tap);
    }
  }  // namespace

  bool should_block(std::uint32_t type, std::int64_t keycode, std::int64_t source_pid, std::int64_t own_pid) {
    if (source_pid == own_pid) {
      return false;
    }
    switch (type) {
      case kCGEventKeyDown:
      case kCGEventKeyUp:
        return keycode != kVK_Escape;
      case kCGEventFlagsChanged:
        return keycode != kVK_Command && keycode != kVK_RightCommand;
      default:
        return true;
    }
  }

  bool start() {
    std::unique_lock lock {g_mutex};
    if (g_thread.joinable()) {
      return g_active;
    }

    std::promise<bool> ready;
    auto installed {ready.get_future()};
    g_blocked = 0;
    g_logged_first = false;
    g_stop = false;
    g_thread = std::thread {run_tap, std::move(ready)};
    // run_tap takes the mutex once the tap exists.
    lock.unlock();
    const bool ok {installed.get()};
    lock.lock();

    if (!ok) {
      g_thread.join();
      BOOST_LOG(warning) << "Could not block the Mac's keyboard and trackpad while streaming; grant MacShine the Accessibility permission"sv;
      return false;
    }
    g_active = true;
    BOOST_LOG(info) << "Ignoring the Mac's keyboard and trackpad while streaming; Cmd+Esc ends the stream"sv;
    return true;
  }

  void stop() {
    std::thread thread;
    {
      std::lock_guard lock {g_mutex};
      if (!g_thread.joinable()) {
        return;
      }
      g_stop = true;
      if (g_run_loop) {
        CFRunLoopStop(g_run_loop);
      }
      thread = std::move(g_thread);
    }
    thread.join();
    g_active = false;
    BOOST_LOG(info) << "Local keyboard and trackpad input restored ("sv << g_blocked.load() << " events ignored)"sv;
  }

  bool active() {
    return g_active;
  }
}  // namespace platf::local_input_block
