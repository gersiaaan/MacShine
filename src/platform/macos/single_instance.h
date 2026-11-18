/**
 * @file src/platform/macos/single_instance.h
 * @brief Declarations for keeping a single Sunshine instance running on macOS.
 */
#pragma once

// standard includes
#include <filesystem>

namespace platf::single_instance {

  /**
   * @brief Exclusive lock that marks the running Sunshine instance.
   *
   * The lock is an advisory `flock` on a file, so the system releases it as soon as the owning
   * process exits or crashes; a stale lock file never blocks the next start. The descriptor is
   * close-on-exec, so helper processes do not keep the lock alive after Sunshine exits.
   */
  class lock_t {
  public:
    lock_t() = default;

    /**
     * @brief Take ownership of a locked file descriptor.
     *
     * @param fd Descriptor that holds the lock.
     */
    explicit lock_t(int fd);

    lock_t(const lock_t &) = delete;
    lock_t &operator=(const lock_t &) = delete;

    /**
     * @brief Move the lock from another instance.
     *
     * @param other Lock to move from; it no longer holds the lock afterwards.
     */
    lock_t(lock_t &&other) noexcept;

    /**
     * @brief Move the lock from another instance, releasing any lock held here.
     *
     * @param other Lock to move from; it no longer holds the lock afterwards.
     * @return This lock.
     */
    lock_t &operator=(lock_t &&other) noexcept;

    /**
     * @brief Release the lock.
     */
    ~lock_t();

    /**
     * @brief Report whether this object holds the lock.
     *
     * @return True when the lock is held.
     */
    [[nodiscard]] bool held() const;

  private:
    int fd_ {-1};  ///< Locked descriptor, or -1 when no lock is held.
  };

  /**
   * @brief Outcome of trying to take the instance lock.
   */
  enum class acquire_result_e {
    acquired,  ///< This process is now the only instance.
    already_running,  ///< Another process holds the lock.
    error  ///< The lock file could not be opened or locked.
  };

  /**
   * @brief Try to take the instance lock without waiting.
   *
   * Missing parent directories of the lock file are created.
   *
   * @param path Lock file path.
   * @param lock Receives the lock when it is acquired.
   * @return Whether the lock was acquired, is held by another process, or failed.
   */
  acquire_result_e try_acquire(const std::filesystem::path &path, lock_t &lock);

}  // namespace platf::single_instance
