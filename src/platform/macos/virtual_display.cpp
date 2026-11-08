/**
 * @file src/platform/macos/virtual_display.cpp
 * @brief Per-session macOS virtual displays, backed by the sunshine_vd_helper process.
 *
 * The CGVirtualDisplay object must live in a process with a serviced main dispatch queue and a
 * clean WindowServer context, so it is owned by a small helper process instead of Sunshine itself.
 * Sunshine keeps the helper's stdin open as a lifeline: if Sunshine exits or crashes, the helper
 * sees EOF and the display disappears with it.
 */
// standard includes
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

// platform includes
#include <fcntl.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

// local includes
#include "src/logging.h"
#include "virtual_display.h"

extern char **environ;

using namespace std::literals;

namespace platf::virtual_display {
  namespace {
    constexpr auto HELPER_NAME {"sunshine_vd_helper"};
    constexpr auto READY_TIMEOUT {10s};

    struct helper_state_t {
      pid_t pid {-1};
      int stdin_fd {-1};  ///< Write end of the helper's stdin, the lifeline.
      std::thread reader;  ///< Reads the helper's output and forwards it to the log.
      std::string display_id;
      request_t request;  ///< Parameters the display was created with.

      std::mutex ready_mutex;
      std::condition_variable ready_cv;
      bool ready {false};
      bool exited {false};
      bool stopping {false};  ///< Sunshine asked the helper to exit, so it restores the host displays itself.
      std::string ready_display_id;
    };

    std::mutex g_mutex;
    std::unique_ptr<helper_state_t> g_helper;

    std::filesystem::path helper_path() {
      std::array<char, PATH_MAX> executable {};
      auto size {static_cast<uint32_t>(executable.size())};
      if (_NSGetExecutablePath(executable.data(), &size) != 0) {
        return {};
      }
      std::array<char, PATH_MAX> resolved {};
      if (!realpath(executable.data(), resolved.data())) {
        return std::filesystem::path {executable.data()}.parent_path() / HELPER_NAME;
      }
      return std::filesystem::path {resolved.data()}.parent_path() / HELPER_NAME;
    }

    void read_helper_output(helper_state_t *state, int read_fd) {
      std::string pending;
      std::array<char, 512> buffer {};
      while (true) {
        const auto n {read(read_fd, buffer.data(), buffer.size())};
        if (n < 0 && errno == EINTR) {
          continue;
        }
        if (n <= 0) {
          break;
        }
        pending.append(buffer.data(), static_cast<std::size_t>(n));

        std::size_t newline;
        while ((newline = pending.find('\n')) != std::string::npos) {
          const auto line {pending.substr(0, newline)};
          pending.erase(0, newline + 1);

          if (line.rfind("READY ", 0) == 0) {
            std::istringstream fields {line.substr(6)};
            std::string id;
            fields >> id;
            {
              std::lock_guard lock {state->ready_mutex};
              state->ready = true;
              state->ready_display_id = id;
            }
            state->ready_cv.notify_all();
            BOOST_LOG(info) << "Virtual display ready: "sv << line.substr(6);
          } else if (!line.empty()) {
            BOOST_LOG(info) << line;
          }
        }
      }
      close(read_fd);

      bool stopping {false};
      {
        std::lock_guard lock {state->ready_mutex};
        state->exited = true;
        stopping = state->stopping;
      }
      state->ready_cv.notify_all();

      // A helper that died on its own (crash, SIGKILL) could not turn the host displays back on.
      if (!stopping && state->request.disable_others) {
        BOOST_LOG(warning) << "Virtual display helper exited unexpectedly, turning host displays back on"sv;
        restore_host_displays();
      }
    }

    void stop_helper_unlocked() {
      if (!g_helper) {
        return;
      }
      auto &helper {*g_helper};
      {
        std::lock_guard ready_lock {helper.ready_mutex};
        helper.stopping = true;
      }

      if (helper.stdin_fd >= 0) {
        close(helper.stdin_fd);  // EOF on the lifeline makes the helper exit on its own
        helper.stdin_fd = -1;
      }

      if (helper.pid > 0) {
        int status {};
        bool reaped {false};
        for (int i = 0; i < 30 && !reaped; ++i) {
          reaped = waitpid(helper.pid, &status, WNOHANG) == helper.pid;
          if (!reaped) {
            std::this_thread::sleep_for(50ms);
          }
        }
        if (!reaped) {
          kill(helper.pid, SIGTERM);
          for (int i = 0; i < 20 && !reaped; ++i) {
            reaped = waitpid(helper.pid, &status, WNOHANG) == helper.pid;
            if (!reaped) {
              std::this_thread::sleep_for(50ms);
            }
          }
        }
        if (!reaped) {
          kill(helper.pid, SIGKILL);
          waitpid(helper.pid, &status, 0);
          if (helper.request.disable_others) {
            restore_host_displays();
          }
        }
      }

      if (helper.reader.joinable()) {
        helper.reader.join();
      }

      if (!helper.display_id.empty()) {
        BOOST_LOG(info) << "Removed virtual display "sv << helper.display_id;
      }
      g_helper.reset();
    }
  }  // namespace

  int display_refresh_rate(int client_fps) {
    if (client_fps <= 0) {
      return client_fps;
    }
    return std::max(client_fps, std::min(client_fps * 2, MAX_REFRESH_RATE));
  }

  std::pair<int, int> display_pixel_size(const request_t &request) {
    // The helper accepts up to 8192 pixels per side.
    constexpr int MAX_PIXELS {8192};
    int scale {std::clamp(request.scale_percent, MIN_SCALE_PERCENT, MAX_SCALE_PERCENT)};
    const int largest_side {std::max(request.width, request.height)};
    if (largest_side > 0 && largest_side * scale / 100 > MAX_PIXELS) {
      scale = MAX_PIXELS * 100 / largest_side;
    }

    const auto scaled = [&](int pixels) {
      const int value {(pixels * scale + 50) / 100};
      return request.hidpi ? value / 2 * 2 : value;
    };
    return {scaled(request.width), scaled(request.height)};
  }

  std::vector<size_option_t> size_options(const request_t &request) {
    // The helper rejects displays below this size.
    constexpr int MIN_WIDTH {320};
    constexpr int MIN_HEIGHT {200};

    std::vector<size_option_t> options;
    const auto add = [&](int percent, bool required) {
      auto sized {request};
      sized.scale_percent = percent;
      const auto [width, height] {display_pixel_size(sized)};
      if (!required && (width < MIN_WIDTH || height < MIN_HEIGHT)) {
        return;
      }
      const bool duplicate {std::ranges::any_of(options, [&](const size_option_t &option) {
        return option.width == width && option.height == height;
      })};
      if (!duplicate) {
        options.push_back({percent, width, height});
      }
    };

    add(std::clamp(request.scale_percent, MIN_SCALE_PERCENT, MAX_SCALE_PERCENT), true);
    for (const int percent : SIZE_PRESETS) {
      add(percent, false);
    }
    std::ranges::sort(options, {}, &size_option_t::percent);
    return options;
  }

  std::vector<std::string> helper_arguments(const std::string &helper, const request_t &request) {
    const auto [width, height] {display_pixel_size(request)};
    std::string sizes;
    for (const auto &option : size_options(request)) {
      if (!sizes.empty()) {
        sizes += ',';
      }
      sizes += std::to_string(option.percent) + ':' + std::to_string(option.width) + 'x' + std::to_string(option.height);
    }
    return {
      helper,
      "--width",
      std::to_string(width),
      "--height",
      std::to_string(height),
      "--fps",
      std::to_string(display_refresh_rate(request.fps)),
      "--hidpi",
      request.hidpi ? "1" : "0",
      "--primary",
      request.make_primary ? "1" : "0",
      "--disable-others",
      request.disable_others ? "1" : "0",
      "--scale",
      std::to_string(std::clamp(request.scale_percent, MIN_SCALE_PERCENT, MAX_SCALE_PERCENT)),
      "--sizes",
      sizes,
      "--serial",
      std::to_string(request.serial),
      "--name",
      request.name,
    };
  }

  void restore_host_displays() {
    const auto path {helper_path()};
    if (path.empty() || access(path.c_str(), X_OK) != 0) {
      return;
    }

    std::array<char *, 3> argv {const_cast<char *>(path.c_str()), const_cast<char *>("--restore"), nullptr};
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
    pid_t pid {-1};
    const int spawn_result {posix_spawn(&pid, path.c_str(), nullptr, &attributes, argv.data(), environ)};
    posix_spawnattr_destroy(&attributes);
    if (spawn_result != 0) {
      BOOST_LOG(error) << "Failed to run the virtual display helper to restore host displays: "sv << spawn_result;
      return;
    }

    int status {};
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      BOOST_LOG(error) << "Could not turn host displays back on; log out and back in to restore them"sv;
    }
  }

  std::uint32_t serial_for_client(const std::string &client_key) {
    // FNV-1a, 32 bit
    std::uint32_t hash {2166136261u};
    for (const unsigned char c : client_key) {
      hash ^= c;
      hash *= 16777619u;
    }
    return hash == 0 ? 1 : hash;
  }

  bool wake_for_stream() {
    IOPMAssertionID assertion {kIOPMNullAssertionID};
    if (IOPMAssertionDeclareUserActivity(CFSTR("MacShine stream started"), kIOPMUserActiveLocal, &assertion) != kIOReturnSuccess) {
      return false;
    }
    IOPMAssertionRelease(assertion);
    return true;
  }

  std::optional<std::string> create(const request_t &request) {
    // An asleep Mac shows a black virtual display until someone touches the Mac; a connecting
    // client counts as that activity, so the stream starts with a picture (the login window if locked).
    if (!wake_for_stream()) {
      BOOST_LOG(warning) << "Could not wake the Mac's displays for the stream"sv;
    }

    std::lock_guard lock {g_mutex};

    // A client resuming with the same settings keeps its display, so windows stay where they are.
    if (g_helper && !g_helper->display_id.empty()) {
      const auto &current {g_helper->request};
      bool alive {false};
      {
        std::lock_guard ready_lock {g_helper->ready_mutex};
        alive = !g_helper->exited;
      }
      if (alive && current.width == request.width && current.height == request.height && current.fps == request.fps && current.hidpi == request.hidpi && current.make_primary == request.make_primary && current.disable_others == request.disable_others && current.scale_percent == request.scale_percent && current.serial == request.serial) {
        BOOST_LOG(info) << "Reusing virtual display "sv << g_helper->display_id;
        return g_helper->display_id;
      }
    }
    stop_helper_unlocked();

    const auto path {helper_path()};
    if (path.empty() || access(path.c_str(), X_OK) != 0) {
      BOOST_LOG(error) << "Virtual display helper not found or not executable: "sv << path.string();
      return std::nullopt;
    }

    std::array<int, 2> in_pipe {-1, -1};
    std::array<int, 2> out_pipe {-1, -1};
    if (pipe(in_pipe.data()) != 0 || pipe(out_pipe.data()) != 0) {
      BOOST_LOG(error) << "Failed to create pipes for the virtual display helper: "sv << errno;
      for (const int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1]}) {
        if (fd >= 0) {
          close(fd);
        }
      }
      return std::nullopt;
    }

    const auto args {helper_arguments(path.string(), request)};
    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (const auto &arg : args) {
      argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDERR_FILENO);

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    // Do not leak Sunshine's sockets (ASIO does not use O_CLOEXEC) into the helper.
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);

    pid_t pid {-1};
    const int spawn_result {posix_spawn(&pid, path.c_str(), &actions, &attributes, argv.data(), environ)};
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(in_pipe[0]);
    close(out_pipe[1]);

    if (spawn_result != 0) {
      BOOST_LOG(error) << "Failed to start the virtual display helper: "sv << spawn_result;
      close(in_pipe[1]);
      close(out_pipe[0]);
      return std::nullopt;
    }
    fcntl(in_pipe[1], F_SETFD, FD_CLOEXEC);
    // Size reports must never block Sunshine or kill it with SIGPIPE when the helper is gone.
    fcntl(in_pipe[1], F_SETFL, fcntl(in_pipe[1], F_GETFL) | O_NONBLOCK);
    fcntl(in_pipe[1], F_SETNOSIGPIPE, 1);
    fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);

    g_helper = std::make_unique<helper_state_t>();
    g_helper->request = request;
    g_helper->pid = pid;
    g_helper->stdin_fd = in_pipe[1];
    g_helper->reader = std::thread {read_helper_output, g_helper.get(), out_pipe[0]};

    BOOST_LOG(info) << "Creating virtual display "sv << request.width << 'x' << request.height << '@' << request.fps
                    << " (display "sv << display_refresh_rate(request.fps) << " Hz)"sv
                    << (request.hidpi ? " (HiDPI)"sv : ""sv) << " at "sv << request.scale_percent << "% for ["sv << request.name << ']';

    std::string display_id;
    {
      std::unique_lock ready_lock {g_helper->ready_mutex};
      g_helper->ready_cv.wait_for(ready_lock, READY_TIMEOUT, [] {
        return g_helper->ready || g_helper->exited;
      });
      if (g_helper->ready) {
        display_id = g_helper->ready_display_id;
      }
    }

    if (display_id.empty()) {
      BOOST_LOG(error) << "Virtual display helper did not become ready"sv;
      stop_helper_unlocked();
      return std::nullopt;
    }

    g_helper->display_id = display_id;
    return display_id;
  }

  void destroy() {
    std::lock_guard lock {g_mutex};
    stop_helper_unlocked();
  }

  std::string active_display_id() {
    std::lock_guard lock {g_mutex};
    if (!g_helper) {
      return {};
    }
    std::lock_guard ready_lock {g_helper->ready_mutex};
    return g_helper->exited ? std::string {} : g_helper->display_id;
  }

  std::string size_message(int pixel_width, int pixel_height, int points_width, int points_height) {
    return "SIZE "s + std::to_string(pixel_width) + ' ' + std::to_string(pixel_height) + ' ' + std::to_string(points_width) + ' ' + std::to_string(points_height) + '\n';
  }

  void report_size(const std::string &display_id, int pixel_width, int pixel_height, int points_width, int points_height) {
    if (display_id.empty() || pixel_width <= 0 || pixel_height <= 0 || points_width <= 0 || points_height <= 0) {
      return;
    }
    std::lock_guard lock {g_mutex};
    if (!g_helper || g_helper->stdin_fd < 0 || g_helper->display_id != display_id) {
      return;
    }
    const auto message {size_message(pixel_width, pixel_height, points_width, points_height)};
    // A full pipe or a helper that just exited only loses this report; the next one is sent on the next change.
    [[maybe_unused]] const auto written {write(g_helper->stdin_fd, message.data(), message.size())};
  }
}  // namespace platf::virtual_display
