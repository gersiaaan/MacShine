/**
 * @file src/platform/macos/sc_video.m
 * @brief ScreenCaptureKit display capture with the AVVideo interface (MacShine).
 *
 * Compiled with ARC (see cmake/compile_definitions/macos.cmake).
 */
// local includes
#import "sc_video.h"

// platform includes
#import <ScreenCaptureKit/ScreenCaptureKit.h>

API_AVAILABLE(macos(12.3))
@interface SCVideo () <SCStreamOutput, SCStreamDelegate>
@end

@implementation SCVideo {
  SCDisplay *_scDisplay;
  SCStream *_stream;
  dispatch_queue_t _queue;
  dispatch_source_t _redeliverTimer;
  FrameCallbackBlock _callback;
  dispatch_semaphore_t _signal;
  CMSampleBufferRef _lastSample;
  uint64_t _lastDeliveryNs;
}

+ (BOOL)isAvailable {
  if (@available(macOS 12.3, *)) {
    return NSClassFromString(@"SCStream") != nil;
  }
  return NO;
}

- (id)initWithDisplay:(CGDirectDisplayID)displayID frameRate:(int)frameRate {
  if (@available(macOS 12.3, *)) {
    // Deliberately skip AVVideo's initializer: it would start an AVCaptureScreenInput session.
    self = [super init];
    if (!self) {
      return nil;
    }

    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayID);
    if (!mode) {
      return nil;
    }
    self.displayID = displayID;
    self.pixelFormat = kCVPixelFormatType_32BGRA;
    self.frameWidth = (int) CGDisplayModeGetPixelWidth(mode);
    self.frameHeight = (int) CGDisplayModeGetPixelHeight(mode);
    self.minFrameDuration = CMTimeMake(1, frameRate > 0 ? frameRate : 60);
    CGDisplayModeRelease(mode);

    // Find the SCDisplay for this display id. Requires Screen Recording permission.
    __block SCDisplay *found = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                               onScreenWindowsOnly:YES
                                                 completionHandler:^(SCShareableContent *content, NSError *error) {
                                                   if (!error) {
                                                     for (SCDisplay *display in content.displays) {
                                                       if (display.displayID == displayID) {
                                                         found = display;
                                                         break;
                                                       }
                                                     }
                                                   } else {
                                                     NSLog(@"[SCVideo] shareable content error: %@", error);
                                                   }
                                                   dispatch_semaphore_signal(done);
                                                 }];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0 || !found) {
      NSLog(@"[SCVideo] display %u not available to ScreenCaptureKit", displayID);
      return nil;
    }
    _scDisplay = found;

    dispatch_queue_attr_t attributes = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0);
    _queue = dispatch_queue_create("macshine.sckit.capture", attributes);
    return self;
  }
  return nil;
}

- (void)dealloc {
  // Never retain self from dealloc: use an unretained reference for the final synchronous stop.
  __unsafe_unretained SCVideo *unsafeSelf = self;
  if (_queue) {
    dispatch_sync(_queue, ^{
      [unsafeSelf finishCapture];
    });
  }
  if (_lastSample) {
    CFRelease(_lastSample);
    _lastSample = NULL;
  }
}

- (dispatch_semaphore_t)capture:(FrameCallbackBlock)frameCallback {
  if (@available(macOS 12.3, *)) {
    @synchronized(self) {
      [self stopStreamWaiting:YES];

      SCStreamConfiguration *configuration = [[SCStreamConfiguration alloc] init];
      configuration.width = (size_t) self.frameWidth;
      configuration.height = (size_t) self.frameHeight;
      configuration.pixelFormat = self.pixelFormat;
      configuration.minimumFrameInterval = self.minFrameDuration;
      configuration.showsCursor = YES;
      configuration.queueDepth = 5;

      SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:_scDisplay excludingWindows:@[]];
      SCStream *stream = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:self];

      NSError *error = nil;
      if (![stream addStreamOutput:self type:SCStreamOutputTypeScreen sampleHandlerQueue:_queue error:&error]) {
        NSLog(@"[SCVideo] addStreamOutput failed: %@", error);
        return nil;
      }

      dispatch_semaphore_t signal = dispatch_semaphore_create(0);
      dispatch_sync(_queue, ^{
        self->_callback = [frameCallback copy];
        self->_signal = signal;
        self->_lastDeliveryNs = 0;
        if (self->_lastSample) {
          CFRelease(self->_lastSample);
          self->_lastSample = NULL;
        }
      });

      dispatch_semaphore_t started = dispatch_semaphore_create(0);
      __block NSError *startError = nil;
      [stream startCaptureWithCompletionHandler:^(NSError *err) {
        startError = err;
        dispatch_semaphore_signal(started);
      }];
      if (dispatch_semaphore_wait(started, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0 || startError) {
        NSLog(@"[SCVideo] startCapture failed: %@", startError);
        dispatch_sync(_queue, ^{
          self->_callback = nil;
          self->_signal = nil;
        });
        return nil;
      }
      _stream = stream;

      // Idle screens produce no new frames; re-deliver the last frame so consumers never starve.
      _redeliverTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _queue);
      const uint64_t interval = 250 * NSEC_PER_MSEC;
      dispatch_source_set_timer(_redeliverTimer, dispatch_time(DISPATCH_TIME_NOW, (int64_t) interval), interval, 20 * NSEC_PER_MSEC);
      __weak SCVideo *weakSelf = self;
      dispatch_source_set_event_handler(_redeliverTimer, ^{
        SCVideo *strongSelf = weakSelf;
        if (!strongSelf || !strongSelf->_lastSample) {
          return;
        }
        if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - strongSelf->_lastDeliveryNs >= interval) {
          [strongSelf deliverSample:strongSelf->_lastSample];
        }
      });
      dispatch_resume(_redeliverTimer);

      return signal;
    }
  }
  return nil;
}

/** Must run on _queue. */
- (void)deliverSample:(CMSampleBufferRef)sample {
  FrameCallbackBlock callback = _callback;
  if (!callback) {
    return;
  }
  _lastDeliveryNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  if (!callback(sample)) {
    [self finishCapture];
  }
}

/** Must run on _queue: end the current capture and wake the waiter. */
- (void)finishCapture {
  dispatch_semaphore_t signal = _signal;
  _callback = nil;
  _signal = nil;
  if (_redeliverTimer) {
    dispatch_source_cancel(_redeliverTimer);
    _redeliverTimer = nil;
  }
  SCStream *stream = _stream;
  _stream = nil;
  if (stream) {
    if (@available(macOS 12.3, *)) {
      [stream stopCaptureWithCompletionHandler:^(NSError *error) {
      }];
    }
  }
  if (signal) {
    dispatch_semaphore_signal(signal);
  }
}

- (void)stopStreamWaiting:(BOOL)wait {
  if (!_queue) {
    return;
  }
  void (^stop)(void) = ^{
    [self finishCapture];
  };
  if (wait) {
    dispatch_sync(_queue, stop);
  } else {
    dispatch_async(_queue, stop);
  }
}

#pragma mark - SCStreamOutput

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type API_AVAILABLE(macos(12.3)) {
  if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sampleBuffer)) {
    return;
  }

  // Only complete frames carry new pixels; idle/blank/suspended frames have no usable image.
  CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
  if (attachments && CFArrayGetCount(attachments) > 0) {
    NSDictionary *info = (__bridge NSDictionary *) CFArrayGetValueAtIndex(attachments, 0);
    NSNumber *status = info[SCStreamFrameInfoStatus];
    if (status && status.integerValue != SCFrameStatusComplete) {
      return;
    }
  }
  if (!CMSampleBufferGetImageBuffer(sampleBuffer)) {
    return;
  }

  if (_lastSample) {
    CFRelease(_lastSample);
  }
  _lastSample = (CMSampleBufferRef) CFRetain(sampleBuffer);
  [self deliverSample:sampleBuffer];
}

#pragma mark - SCStreamDelegate

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error API_AVAILABLE(macos(12.3)) {
  NSLog(@"[SCVideo] stream stopped: %@", error);
  dispatch_async(_queue, ^{
    if (self->_stream == stream) {
      [self finishCapture];
    }
  });
}

@end
