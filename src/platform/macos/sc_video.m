/**
 * @file src/platform/macos/sc_video.m
 * @brief ScreenCaptureKit display capture with the AVVideo interface (MacShine).
 *
 * Compiled with ARC (see cmake/compile_definitions/macos.cmake).
 */
// local includes
#import "sc_video.h"

// platform includes
#import <os/lock.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

/**
 * @brief One capture: request: the frame callback and the semaphore signalled when it ends.
 */
@interface SCVideoConsumer: NSObject
@property (nonatomic, copy) FrameCallbackBlock callback;  ///< Frame callback; returns false to stop.
@property (nonatomic, strong) dispatch_semaphore_t signal;  ///< Signalled when this consumer ends.
@end

@implementation SCVideoConsumer
@end

/** Marks the capture queue, so dealloc can tell whether it is running on it. */
static char kSCVideoQueueKey;

API_AVAILABLE(macos(12.3))
@interface SCVideo () <SCStreamOutput, SCStreamDelegate>
@end

/*
 * Several capture: requests can be active at once (the streaming capture loop and an encoder's
 * dummy_img probe use the same display object), so they all share one SCStream and each frame is
 * handed to every consumer. Consumers are guarded by _lock, never by _queue: a consumer callback can
 * block on _queue while it waits for the encoder, and capture: must not wait behind it.
 */
@implementation SCVideo {
  SCDisplay *_scDisplay;
  SCStream *_stream;
  dispatch_queue_t _queue;
  dispatch_source_t _redeliverTimer;
  os_unfair_lock _lock;
  NSMutableArray<SCVideoConsumer *> *_consumers;
  CMSampleBufferRef _lastSample;
  uint64_t _lastDeliveryNs;
  size_t _streamWidth;
  size_t _streamHeight;
  OSType _streamPixelFormat;
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

    _lock = OS_UNFAIR_LOCK_INIT;
    _consumers = [NSMutableArray array];
    dispatch_queue_attr_t attributes = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0);
    _queue = dispatch_queue_create("macshine.sckit.capture", attributes);
    dispatch_queue_set_specific(_queue, &kSCVideoQueueKey, &kSCVideoQueueKey, NULL);
    return self;
  }
  return nil;
}

- (void)dealloc {
  if (@available(macOS 12.3, *)) {
    [self stopStream];
  }
  [self endAllConsumers];
  if (_queue && dispatch_get_specific(&kSCVideoQueueKey) == NULL) {
    // Let frame deliveries that were already queued finish before the ivars go away.
    dispatch_sync(_queue, ^ {});
  }
  if (_lastSample) {
    CFRelease(_lastSample);
    _lastSample = NULL;
  }
}

- (dispatch_semaphore_t)capture:(FrameCallbackBlock)frameCallback {
  if (@available(macOS 12.3, *)) {
    @synchronized(self) {
      if (![self ensureStream]) {
        return nil;
      }

      SCVideoConsumer *consumer = [[SCVideoConsumer alloc] init];
      consumer.callback = frameCallback;
      consumer.signal = dispatch_semaphore_create(0);

      os_unfair_lock_lock(&_lock);
      [_consumers addObject:consumer];
      const BOOL hasLastSample = _lastSample != NULL;
      os_unfair_lock_unlock(&_lock);

      // Hand the newest frame to the new consumer right away: an idle display may not produce another.
      if (hasLastSample) {
        __weak SCVideo *weakSelf = self;
        dispatch_async(_queue, ^{
          SCVideo *strongSelf = weakSelf;
          if (strongSelf) {
            [strongSelf redeliverTo:consumer];
          }
        });
      }

      return consumer.signal;
    }
  }
  return nil;
}

/**
 * @brief Start the shared stream, or reconfigure it when the requested frame format changed.
 * @return YES when a stream with the current configuration is running.
 */
- (BOOL)ensureStream API_AVAILABLE(macos(12.3)) {
  const size_t width = (size_t) self.frameWidth;
  const size_t height = (size_t) self.frameHeight;
  const OSType pixelFormat = self.pixelFormat;

  SCStreamConfiguration *configuration = [[SCStreamConfiguration alloc] init];
  configuration.width = width;
  configuration.height = height;
  configuration.pixelFormat = pixelFormat;
  configuration.minimumFrameInterval = self.minFrameDuration;
  configuration.showsCursor = YES;
  configuration.queueDepth = 5;

  if (_stream) {
    if (_streamWidth == width && _streamHeight == height && _streamPixelFormat == pixelFormat) {
      return YES;
    }

    dispatch_semaphore_t updated = dispatch_semaphore_create(0);
    __block NSError *updateError = nil;
    [_stream updateConfiguration:configuration
               completionHandler:^(NSError *err) {
                 updateError = err;
                 dispatch_semaphore_signal(updated);
               }];
    if (dispatch_semaphore_wait(updated, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0 || updateError) {
      NSLog(@"[SCVideo] updateConfiguration failed: %@", updateError);
      return NO;
    }
    [self setStreamFormatWidth:width height:height pixelFormat:pixelFormat];
    return YES;
  }

  SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:_scDisplay excludingWindows:@[]];
  SCStream *stream = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:self];

  NSError *error = nil;
  if (![stream addStreamOutput:self type:SCStreamOutputTypeScreen sampleHandlerQueue:_queue error:&error]) {
    NSLog(@"[SCVideo] addStreamOutput failed: %@", error);
    return NO;
  }

  dispatch_semaphore_t started = dispatch_semaphore_create(0);
  __block NSError *startError = nil;
  [stream startCaptureWithCompletionHandler:^(NSError *err) {
    startError = err;
    dispatch_semaphore_signal(started);
  }];
  if (dispatch_semaphore_wait(started, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0 || startError) {
    NSLog(@"[SCVideo] startCapture failed: %@", startError);
    return NO;
  }
  _stream = stream;
  [self setStreamFormatWidth:width height:height pixelFormat:pixelFormat];

  // Idle screens produce no new frames; re-deliver the last frame so consumers never starve.
  _redeliverTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _queue);
  const uint64_t interval = 250 * NSEC_PER_MSEC;
  dispatch_source_set_timer(_redeliverTimer, dispatch_time(DISPATCH_TIME_NOW, (int64_t) interval), interval, 20 * NSEC_PER_MSEC);
  __weak SCVideo *weakSelf = self;
  dispatch_source_set_event_handler(_redeliverTimer, ^{
    SCVideo *strongSelf = weakSelf;
    if (strongSelf && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - strongSelf->_lastDeliveryNs >= interval) {
      [strongSelf redeliverTo:nil];
    }
  });
  dispatch_resume(_redeliverTimer);
  return YES;
}

/**
 * @brief Record the stream's frame format and drop a last frame that no longer matches it.
 * @param width Frame width in pixels.
 * @param height Frame height in pixels.
 * @param pixelFormat Frame pixel format.
 */
- (void)setStreamFormatWidth:(size_t)width height:(size_t)height pixelFormat:(OSType)pixelFormat {
  _streamWidth = width;
  _streamHeight = height;
  _streamPixelFormat = pixelFormat;

  os_unfair_lock_lock(&_lock);
  CMSampleBufferRef stale = _lastSample;
  _lastSample = NULL;
  os_unfair_lock_unlock(&_lock);
  if (stale) {
    CFRelease(stale);
  }
}

/** Stop the shared stream; consumers are left untouched. */
- (void)stopStream API_AVAILABLE(macos(12.3)) {
  if (_redeliverTimer) {
    dispatch_source_cancel(_redeliverTimer);
    _redeliverTimer = nil;
  }
  SCStream *stream = _stream;
  _stream = nil;
  if (stream) {
    [stream removeStreamOutput:self type:SCStreamOutputTypeScreen error:nil];
    [stream stopCaptureWithCompletionHandler:^(NSError *error) {}];
  }
}

/** End every consumer and wake its waiter. */
- (void)endAllConsumers {
  os_unfair_lock_lock(&_lock);
  NSArray<SCVideoConsumer *> *ended = [_consumers copy];
  [_consumers removeAllObjects];
  os_unfair_lock_unlock(&_lock);
  for (SCVideoConsumer *consumer in ended) {
    dispatch_semaphore_signal(consumer.signal);
  }
}

/**
 * Must run on _queue: re-deliver the last frame.
 * @param only Deliver to this consumer only, or to all consumers when nil.
 */
- (void)redeliverTo:(SCVideoConsumer *)only {
  os_unfair_lock_lock(&_lock);
  CMSampleBufferRef sample = _lastSample ? (CMSampleBufferRef) CFRetain(_lastSample) : NULL;
  os_unfair_lock_unlock(&_lock);
  if (sample) {
    [self deliverSample:sample to:only];
    CFRelease(sample);
  }
}

/**
 * Must run on _queue: hand a frame to consumers and end those whose callback returns false.
 * @param sample Frame to deliver.
 * @param only Deliver to this consumer only, or to all consumers when nil.
 */
- (void)deliverSample:(CMSampleBufferRef)sample to:(SCVideoConsumer *)only {
  os_unfair_lock_lock(&_lock);
  NSArray<SCVideoConsumer *> *targets = only ? ([_consumers containsObject:only] ? @[only] : @[]) : [_consumers copy];
  os_unfair_lock_unlock(&_lock);

  _lastDeliveryNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  for (SCVideoConsumer *consumer in targets) {
    if (consumer.callback(sample)) {
      continue;
    }
    os_unfair_lock_lock(&_lock);
    [_consumers removeObjectIdenticalTo:consumer];
    os_unfair_lock_unlock(&_lock);
    dispatch_semaphore_signal(consumer.signal);
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

  os_unfair_lock_lock(&_lock);
  CMSampleBufferRef previous = _lastSample;
  _lastSample = (CMSampleBufferRef) CFRetain(sampleBuffer);
  os_unfair_lock_unlock(&_lock);
  if (previous) {
    CFRelease(previous);
  }
  [self deliverSample:sampleBuffer to:nil];
}

#pragma mark - SCStreamDelegate

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error API_AVAILABLE(macos(12.3)) {
  NSLog(@"[SCVideo] stream stopped: %@", error);
  @synchronized(self) {
    if (_stream == stream) {
      [self stopStream];
    }
  }
  [self endAllConsumers];
}

@end
