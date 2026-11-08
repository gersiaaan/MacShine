/**
 * @file src/platform/macos/frame_timing.cpp
 * @brief Definitions for converting macOS capture times to Sunshine frame timestamps.
 */
// standard includes
#include <cstdint>

// platform includes
#include <mach/mach_time.h>

// local includes
#include "src/platform/macos/frame_timing.h"

namespace platf::frame_timing {

  namespace {
    /** Frames older than this were re-delivered, not just captured. */
    constexpr std::chrono::milliseconds MAX_CAPTURE_AGE {200};
  }  // namespace

  std::optional<std::chrono::steady_clock::time_point> host_time_to_steady(CMTime host_time) {
    if (!CMTIME_IS_NUMERIC(host_time)) {
      return std::nullopt;
    }

    static const mach_timebase_info_data_t timebase = [] {
      mach_timebase_info_data_t info {};
      mach_timebase_info(&info);
      return info;
    }();

    const auto to_nanoseconds = [](std::uint64_t ticks) {
      return static_cast<std::int64_t>(static_cast<__uint128_t>(ticks) * timebase.numer / timebase.denom);
    };

    // The host clock stops while the Mac sleeps, steady_clock does not; measure how long ago the
    // host time was and step back that far from now.
    const auto host_now {to_nanoseconds(mach_absolute_time())};
    const auto steady_now {std::chrono::steady_clock::now()};
    const auto age {std::chrono::nanoseconds {host_now - to_nanoseconds(CMClockConvertHostTimeToSystemUnits(host_time))}};
    return steady_now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(age);
  }

  std::chrono::steady_clock::time_point capture_timestamp(CMTime presentation_time, std::chrono::steady_clock::time_point now) {
    const auto captured {host_time_to_steady(presentation_time)};
    if (!captured || *captured > now || now - *captured > MAX_CAPTURE_AGE) {
      return now;
    }
    return *captured;
  }

}  // namespace platf::frame_timing
