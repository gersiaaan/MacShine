/**
 * @file src/platform/macos/vt_session.cpp
 * @brief Definitions for making VideoToolbox encode each frame before returning it to FFmpeg.
 */
// standard includes
#include <atomic>
#include <chrono>

// platform includes
#include <dlfcn.h>
#include <VideoToolbox/VideoToolbox.h>

// local includes
#include "src/logging.h"
#include "src/platform/macos/vt_session.h"

namespace {
  std::atomic<unsigned long> g_completed_frames {0};  ///< Frames completed by the wrapper.

  /**
   * @brief Signature of VideoToolbox's VTCompressionSessionEncodeFrame.
   */
  using encode_frame_fn = OSStatus (*)(VTCompressionSessionRef, CVImageBufferRef, CMTime, CMTime, CFDictionaryRef, void *, VTEncodeInfoFlags *);

  /**
   * @brief Find VideoToolbox's own VTCompressionSessionEncodeFrame behind this definition.
   * @return The real function, or nullptr if it cannot be found.
   */
  encode_frame_fn real_encode_frame() {
    static const auto real {reinterpret_cast<encode_frame_fn>(dlsym(RTLD_NEXT, "VTCompressionSessionEncodeFrame"))};
    return real;
  }
}  // namespace

namespace platf::vt_session {

  unsigned long completed_frames() {
    return g_completed_frames.load();
  }

}  // namespace platf::vt_session

/**
 * @brief Encode a frame with VideoToolbox and wait until it is finished.
 *
 * Replaces the VideoToolbox function for the statically linked FFmpeg (see vt_session.h).
 *
 * @param session Compression session.
 * @param imageBuffer Frame to encode.
 * @param presentationTimeStamp Presentation time of the frame.
 * @param duration Frame duration.
 * @param frameProperties Per-frame encoder properties.
 * @param sourceFrameRefcon Value passed to the output callback.
 * @param infoFlagsOut Receives encoding information flags.
 * @return VideoToolbox status of the encode, or of completing the frame.
 */
extern "C" OSStatus VTCompressionSessionEncodeFrame(
  VTCompressionSessionRef session,
  CVImageBufferRef imageBuffer,
  CMTime presentationTimeStamp,
  CMTime duration,
  CFDictionaryRef frameProperties,
  void *sourceFrameRefcon,
  VTEncodeInfoFlags *infoFlagsOut
) {
  const auto real {real_encode_frame()};
  if (!real) {
    return kVTParameterErr;
  }

  const auto started {std::chrono::steady_clock::now()};
  const OSStatus status {real(session, imageBuffer, presentationTimeStamp, duration, frameProperties, sourceFrameRefcon, infoFlagsOut)};
  if (status != noErr) {
    return status;
  }

  // Returns once the output callback ran for this frame, so FFmpeg finds the packet right away.
  const OSStatus completed {VTCompressionSessionCompleteFrames(session, presentationTimeStamp)};
  if (completed == noErr) {
    g_completed_frames.fetch_add(1);
    thread_local logging::min_max_avg_periodic_logger<double> encode_logger(debug, "VideoToolbox encode time", "ms");
    encode_logger.collect_and_log(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
  }
  return completed;
}
