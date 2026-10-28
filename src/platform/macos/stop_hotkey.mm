/**
 * @file src/platform/macos/stop_hotkey.mm
 * @brief Global host hotkey that ends every stream (MacShine).
 *
 * Uses Carbon's RegisterEventHotKey: it delivers the key combination system-wide without the
 * Input Monitoring permission that an event tap would need.
 */
// platform includes
#include <Carbon/Carbon.h>
#include <dispatch/dispatch.h>

// standard includes
#include <mutex>
#include <thread>
#include <utility>

// local includes
#include "src/logging.h"
#include "stop_hotkey.h"

using namespace std::literals;

namespace platf::stop_hotkey {
  namespace {
    constexpr OSType HOTKEY_SIGNATURE {'MSHK'};

    std::mutex g_mutex;
    EventHotKeyRef g_hotkey {};
    EventHandlerRef g_handler {};
    std::function<void()> g_on_pressed;

    OSStatus handle_hotkey(EventHandlerCallRef /*next*/, EventRef /*event*/, void * /*user_data*/) {
      std::function<void()> on_pressed;
      {
        std::lock_guard lock {g_mutex};
        on_pressed = g_on_pressed;
      }
      if (on_pressed) {
        BOOST_LOG(info) << "Stop-stream hotkey pressed"sv;
        // Ending sessions joins their threads; keep that off the main (tray) thread.
        std::thread {std::move(on_pressed)}.detach();
      }
      return noErr;
    }
  }  // namespace

  bool register_hotkey(std::function<void()> on_pressed) {
    {
      std::lock_guard lock {g_mutex};
      g_on_pressed = std::move(on_pressed);
    }

    __block bool registered {false};
    // Carbon event targets belong to the main thread.
    auto register_on_main = ^{
      std::lock_guard lock {g_mutex};
      if (g_hotkey) {
        registered = true;
        return;
      }

      const EventTypeSpec event_type {kEventClassKeyboard, kEventHotKeyPressed};
      if (InstallApplicationEventHandler(&handle_hotkey, 1, &event_type, nullptr, &g_handler) != noErr) {
        g_handler = nullptr;
        return;
      }

      const EventHotKeyID hotkey_id {HOTKEY_SIGNATURE, 1};
      if (RegisterEventHotKey(kVK_Escape, cmdKey, hotkey_id, GetApplicationEventTarget(), 0, &g_hotkey) != noErr) {
        g_hotkey = nullptr;
        RemoveEventHandler(g_handler);
        g_handler = nullptr;
        return;
      }
      registered = true;
    };

    if (pthread_main_np()) {
      register_on_main();
    } else {
      dispatch_sync(dispatch_get_main_queue(), register_on_main);
    }

    if (registered) {
      BOOST_LOG(info) << "Press Cmd+Esc on the host to end all streams"sv;
    } else {
      BOOST_LOG(warning) << "Could not register the Cmd+Esc stop-stream hotkey"sv;
    }
    return registered;
  }

  void unregister_hotkey() {
    auto unregister_on_main = ^{
      std::lock_guard lock {g_mutex};
      if (g_hotkey) {
        UnregisterEventHotKey(g_hotkey);
        g_hotkey = nullptr;
      }
      if (g_handler) {
        RemoveEventHandler(g_handler);
        g_handler = nullptr;
      }
      g_on_pressed = nullptr;
    };

    if (pthread_main_np()) {
      unregister_on_main();
    } else {
      dispatch_async(dispatch_get_main_queue(), unregister_on_main);
    }
  }
}  // namespace platf::stop_hotkey
