/**
 * @file src/platform/macos/vt_session.h
 * @brief Declarations for making VideoToolbox encode each frame before returning it to FFmpeg.
 *
 * FFmpeg's VideoToolbox encoder submits a frame and then takes whatever packet is already finished.
 * With hardware frames the new frame is never finished at that moment, so every packet left
 * Sunshine one frame late (or, on an idle screen, only when the next frame arrived) and its capture
 * timestamp was dropped.
 *
 * vt_session.cpp therefore defines VTCompressionSessionEncodeFrame itself. The statically linked
 * FFmpeg binds to that definition; it calls the real VideoToolbox function and then completes the
 * frame, so the packet is ready when FFmpeg looks for it.
 */
#pragma once

namespace platf::vt_session {

  /**
   * @brief Count of frames the wrapper has completed, for tests and diagnostics.
   * @return Number of frames completed since the process started.
   */
  unsigned long completed_frames();

}  // namespace platf::vt_session
