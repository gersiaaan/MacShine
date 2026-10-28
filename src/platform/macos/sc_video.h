/**
 * @file src/platform/macos/sc_video.h
 * @brief ScreenCaptureKit display capture with the AVVideo interface (MacShine).
 */
#pragma once

// local includes
#import "av_video.h"

/**
 * @brief Display capture backed by ScreenCaptureKit.
 *
 * Drop-in replacement for AVVideo: same properties, same capture: contract, so the rest of the
 * macOS capture and encode pipeline is unchanged. Unlike AVCaptureScreenInput, ScreenCaptureKit
 * reliably captures virtual displays. When the screen is idle no new frames arrive, so the last
 * complete frame is re-delivered periodically to keep the stream and encoder probes moving.
 */
@interface SCVideo: AVVideo

/**
 * @brief Whether ScreenCaptureKit can be used on this system.
 * @return YES on macOS 12.3 or newer.
 */
+ (BOOL)isAvailable;

@end
