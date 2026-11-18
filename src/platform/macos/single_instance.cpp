/**
 * @file src/platform/macos/single_instance.cpp
 * @brief Definitions for keeping a single Sunshine instance running on macOS.
 */
// standard includes
#include <cerrno>
#include <system_error>
#include <utility>

// platform includes
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

// local includes
#include "src/platform/macos/single_instance.h"

namespace platf::single_instance {

  lock_t::lock_t(int fd):
      fd_ {fd} {
  }

  lock_t::lock_t(lock_t &&other) noexcept:
      fd_ {std::exchange(other.fd_, -1)} {
  }

  lock_t &lock_t::operator=(lock_t &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) {
        close(fd_);
      }
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }

  lock_t::~lock_t() {
    if (fd_ >= 0) {
      close(fd_);  // closing the only descriptor releases the flock
    }
  }

  bool lock_t::held() const {
    return fd_ >= 0;
  }

  acquire_result_e try_acquire(const std::filesystem::path &path, lock_t &lock) {
    std::error_code ec;
    if (path.has_parent_path()) {
      std::filesystem::create_directories(path.parent_path(), ec);
    }

    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
      return acquire_result_e::error;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
      const int error = errno;
      close(fd);
      return error == EWOULDBLOCK ? acquire_result_e::already_running : acquire_result_e::error;
    }

    lock = lock_t {fd};
    return acquire_result_e::acquired;
  }

}  // namespace platf::single_instance
