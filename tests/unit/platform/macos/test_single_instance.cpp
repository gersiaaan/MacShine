/**
 * @file tests/unit/platform/macos/test_single_instance.cpp
 * @brief Tests for keeping a single Sunshine instance running on macOS.
 */

#ifdef __APPLE__
  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <csignal>
  #include <filesystem>
  #include <string>
  #include <utility>

  // platform includes
  #include <sys/wait.h>
  #include <unistd.h>

  // local includes
  #include "src/platform/macos/single_instance.h"

namespace {
  using platf::single_instance::acquire_result_e;
  using platf::single_instance::lock_t;
  using platf::single_instance::try_acquire;

  /**
   * @brief Create a unique empty directory for one test.
   * @return Directory path.
   */
  std::filesystem::path make_temp_dir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "sunshine_lock_XXXXXX").string();
    const char *dir = mkdtemp(pattern.data());
    return dir ? std::filesystem::path {dir} : std::filesystem::path {};
  }

  /**
   * @brief Try the lock from a forked child, since flock is per open file description.
   * @param path Lock file path.
   * @return The child's result, or acquire_result_e::error if the child failed.
   */
  acquire_result_e try_acquire_in_child(const std::filesystem::path &path) {
    const pid_t child = fork();
    if (child == 0) {
      lock_t lock;
      _exit(static_cast<int>(try_acquire(path, lock)));
    }
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
      return acquire_result_e::error;
    }
    return static_cast<acquire_result_e>(WEXITSTATUS(status));
  }
}  // namespace

TEST(SingleInstanceTest, AcquiresFreeLockAndCreatesMissingDirectories) {
  const auto dir = make_temp_dir();
  ASSERT_FALSE(dir.empty());
  const auto path = dir / "nested" / "sunshine.lock";

  lock_t lock;
  EXPECT_FALSE(lock.held());
  EXPECT_EQ(try_acquire(path, lock), acquire_result_e::acquired);
  EXPECT_TRUE(lock.held());
  EXPECT_TRUE(std::filesystem::exists(path));

  std::filesystem::remove_all(dir);
}

TEST(SingleInstanceTest, SecondProcessSeesAlreadyRunningUntilRelease) {
  const auto dir = make_temp_dir();
  ASSERT_FALSE(dir.empty());
  const auto path = dir / "sunshine.lock";

  {
    lock_t lock;
    ASSERT_EQ(try_acquire(path, lock), acquire_result_e::acquired);
    EXPECT_EQ(try_acquire_in_child(path), acquire_result_e::already_running);
  }

  // The stale lock file left behind does not block the next instance.
  EXPECT_EQ(try_acquire_in_child(path), acquire_result_e::acquired);

  std::filesystem::remove_all(dir);
}

TEST(SingleInstanceTest, MovedLockStaysHeldUntilLastOwnerReleases) {
  const auto dir = make_temp_dir();
  ASSERT_FALSE(dir.empty());
  const auto path = dir / "sunshine.lock";

  lock_t first;
  ASSERT_EQ(try_acquire(path, first), acquire_result_e::acquired);

  lock_t second {std::move(first)};
  EXPECT_FALSE(first.held());
  EXPECT_TRUE(second.held());
  EXPECT_EQ(try_acquire_in_child(path), acquire_result_e::already_running);

  lock_t third;
  third = std::move(second);
  EXPECT_FALSE(second.held());
  EXPECT_TRUE(third.held());
  EXPECT_EQ(try_acquire_in_child(path), acquire_result_e::already_running);

  third = lock_t {};
  EXPECT_FALSE(third.held());
  EXPECT_EQ(try_acquire_in_child(path), acquire_result_e::acquired);

  std::filesystem::remove_all(dir);
}

TEST(SingleInstanceTest, LockIsNotInheritedAcrossExec) {
  const auto dir = make_temp_dir();
  ASSERT_FALSE(dir.empty());
  const auto path = dir / "sunshine.lock";

  lock_t lock;
  ASSERT_EQ(try_acquire(path, lock), acquire_result_e::acquired);

  // A helper started with fork+exec must not keep the lock alive after Sunshine releases it.
  const pid_t helper = fork();
  if (helper == 0) {
    execl("/bin/sleep", "sleep", "5", nullptr);
    _exit(127);
  }
  ASSERT_GT(helper, 0);

  lock = lock_t {};
  EXPECT_EQ(try_acquire_in_child(path), acquire_result_e::acquired);

  kill(helper, SIGKILL);
  waitpid(helper, nullptr, 0);

  std::filesystem::remove_all(dir);
}

TEST(SingleInstanceTest, ReportsErrorWhenLockFileCannotBeOpened) {
  lock_t lock;
  EXPECT_EQ(try_acquire("/dev/null/sunshine.lock", lock), acquire_result_e::error);
  EXPECT_FALSE(lock.held());
}
#endif
