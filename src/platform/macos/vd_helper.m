/**
 * @file src/platform/macos/vd_helper.m
 * @brief Standalone helper that owns one CGVirtualDisplay for the lifetime of a stream.
 *
 * Sunshine spawns this helper when a Moonlight session starts and the macOS virtual display
 * option is enabled. The helper creates a virtual monitor that matches the client's requested
 * resolution and refresh rate, optionally makes it the main display, prints
 * `READY <display id> <pixel width> <pixel height>` on stdout and then keeps the display alive.
 *
 * The display disappears when the helper exits. The helper exits when:
 *   - stdin reaches EOF (Sunshine closed the pipe or died), or
 *   - it receives SIGTERM/SIGINT.
 *
 * The display handling (HiDPI mode enforcement, mirror-set detaching) follows the approach used
 * by OpenDisplay (https://github.com/peetzweg/opendisplay, GPL-3.0). CGVirtualDisplay is a private
 * CoreGraphics API and may break across macOS versions.
 *
 * Compiled with ARC.
 */
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#pragma mark - Private CoreGraphics interface

@class CGVirtualDisplayDescriptor;

@interface CGVirtualDisplayMode: NSObject
@property (readonly, nonatomic) CGFloat refreshRate;
@property (readonly, nonatomic) NSUInteger width;
@property (readonly, nonatomic) NSUInteger height;
- (instancetype)initWithWidth:(NSUInteger)width height:(NSUInteger)height refreshRate:(CGFloat)refreshRate;
@end

@interface CGVirtualDisplaySettings: NSObject
@property (retain, nonatomic) NSArray<CGVirtualDisplayMode *> *modes;
@property (nonatomic) unsigned int hiDPI;
- (instancetype)init;
@end

@interface CGVirtualDisplay: NSObject
@property (readonly, nonatomic) CGDirectDisplayID displayID;
- (instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor *)descriptor;
- (BOOL)applySettings:(CGVirtualDisplaySettings *)settings;
@end

@interface CGVirtualDisplayDescriptor: NSObject
@property (retain, nonatomic) NSString *name;
@property (nonatomic) unsigned int maxPixelsHigh;
@property (nonatomic) unsigned int maxPixelsWide;
@property (nonatomic) CGSize sizeInMillimeters;
@property (nonatomic) unsigned int serialNum;
@property (nonatomic) unsigned int productID;
@property (nonatomic) unsigned int vendorID;
@property (copy, nonatomic) void (^terminationHandler)(id, CGVirtualDisplay *);
- (instancetype)init;
- (void)setDispatchQueue:(dispatch_queue_t)queue;
@end

#pragma mark - State

static CGVirtualDisplay *g_display;
static CGVirtualDisplaySettings *g_settings;
static NSUInteger g_points_wide;
static NSUInteger g_points_high;
static NSUInteger g_scale;
static BOOL g_make_primary;
static BOOL g_ready_sent;
static int g_mode_attempts;

static void log_line(NSString *format, ...) NS_FORMAT_FUNCTION(1, 2);

static void log_line(NSString *format, ...) {
  va_list args;
  va_start(args, format);
  NSString *message = [[NSString alloc] initWithFormat:format arguments:args];
  va_end(args);
  fprintf(stderr, "[vd_helper] %s\n", message.UTF8String);
  fflush(stderr);
}

static unsigned long parse_ulong(const char *value, unsigned long fallback) {
  if (!value || !*value) {
    return fallback;
  }
  char *end = NULL;
  unsigned long parsed = strtoul(value, &end, 10);
  return (end && *end == '\0') ? parsed : fallback;
}

static BOOL display_is_active(CGDirectDisplayID display_id) {
  uint32_t count = 0;
  if (CGGetActiveDisplayList(0, NULL, &count) != kCGErrorSuccess || count == 0) {
    return NO;
  }
  CGDirectDisplayID list[count];
  if (CGGetActiveDisplayList(count, list, &count) != kCGErrorSuccess) {
    return NO;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (list[i] == display_id) {
      return YES;
    }
  }
  return NO;
}

#pragma mark - Display enforcement

/** Detach the virtual display from any mirror set so it acts as its own extended screen. */
static void ensure_not_mirrored(void) {
  const CGDirectDisplayID display_id = g_display.displayID;
  if (CGDisplayIsInMirrorSet(display_id) != 1) {
    return;
  }

  CGDisplayConfigRef config = NULL;
  if (CGBeginDisplayConfiguration(&config) != kCGErrorSuccess) {
    return;
  }
  CGConfigureDisplayMirrorOfDisplay(config, display_id, kCGNullDirectDisplay);

  uint32_t count = 0;
  CGGetActiveDisplayList(0, NULL, &count);
  if (count > 0) {
    CGDirectDisplayID list[count];
    CGGetActiveDisplayList(count, list, &count);
    for (uint32_t i = 0; i < count; ++i) {
      if (list[i] != display_id && CGDisplayMirrorsDisplay(list[i]) == display_id) {
        CGConfigureDisplayMirrorOfDisplay(config, list[i], kCGNullDirectDisplay);
      }
    }
  }

  // Session scope: permanent mirror changes of a virtual display are rejected by WindowServer.
  const CGError err = CGCompleteDisplayConfiguration(config, kCGConfigureForSession);
  log_line(@"display %u was in a mirror set, detached (result %d)", display_id, err);
}

/**
 * Select the mode whose backing pixels match the client exactly.
 * macOS defaults a new HiDPI-capable display to its 1x mode and may restore a stale saved mode,
 * so this is re-checked periodically.
 * @return YES when the target mode is active.
 */
static BOOL select_target_mode(void) {
  const CGDirectDisplayID display_id = g_display.displayID;
  NSDictionary *options = @{(__bridge NSString *) kCGDisplayShowDuplicateLowResolutionModes: @YES};
  CFArrayRef modes = CGDisplayCopyAllDisplayModes(display_id, (__bridge CFDictionaryRef) options);
  if (!modes) {
    return NO;
  }

  CGDisplayModeRef target = NULL;
  for (CFIndex i = 0; i < CFArrayGetCount(modes); ++i) {
    CGDisplayModeRef mode = (CGDisplayModeRef) CFArrayGetValueAtIndex(modes, i);
    if (CGDisplayModeGetWidth(mode) == g_points_wide &&
        CGDisplayModeGetHeight(mode) == g_points_high &&
        CGDisplayModeGetPixelWidth(mode) == g_points_wide * g_scale &&
        CGDisplayModeGetPixelHeight(mode) == g_points_high * g_scale) {
      target = mode;
      break;
    }
  }

  if (!target) {
    CFRelease(modes);
    // The mode list can lag behind applySettings; re-apply once in a while.
    if (++g_mode_attempts % 5 == 0) {
      [g_display applySettings:g_settings];
    }
    return NO;
  }

  BOOL active = NO;
  CGDisplayModeRef current = CGDisplayCopyDisplayMode(display_id);
  if (current) {
    active = CGDisplayModeGetWidth(current) == CGDisplayModeGetWidth(target) &&
             CGDisplayModeGetHeight(current) == CGDisplayModeGetHeight(target) &&
             CGDisplayModeGetPixelWidth(current) == CGDisplayModeGetPixelWidth(target) &&
             CGDisplayModeGetPixelHeight(current) == CGDisplayModeGetPixelHeight(target);
    CGDisplayModeRelease(current);
  }

  if (!active) {
    CGDisplayConfigRef config = NULL;
    if (CGBeginDisplayConfiguration(&config) == kCGErrorSuccess) {
      CGConfigureDisplayWithDisplayMode(config, display_id, target, NULL);
      const CGError err = CGCompleteDisplayConfiguration(config, kCGConfigureForSession);
      log_line(@"selected mode %lux%lu @%lux (result %d)", (unsigned long) g_points_wide, (unsigned long) g_points_high, (unsigned long) g_scale, err);
      active = err == kCGErrorSuccess;
    }
  }

  CFRelease(modes);
  return active;
}

/**
 * Move the virtual display to the global origin, which makes it the main display:
 * the menu bar and the Dock move onto it. Every other display is shifted by the same offset so
 * the arrangement stays intact. Session scope, so nothing is saved permanently.
 */
static void make_main_display(void) {
  const CGDirectDisplayID display_id = g_display.displayID;
  if (CGMainDisplayID() == display_id) {
    return;
  }

  const CGRect bounds = CGDisplayBounds(display_id);
  const int32_t dx = (int32_t) bounds.origin.x;
  const int32_t dy = (int32_t) bounds.origin.y;

  uint32_t count = 0;
  CGGetActiveDisplayList(0, NULL, &count);
  if (count == 0) {
    return;
  }
  CGDirectDisplayID list[count];
  CGGetActiveDisplayList(count, list, &count);

  CGDisplayConfigRef config = NULL;
  if (CGBeginDisplayConfiguration(&config) != kCGErrorSuccess) {
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    // Displays in a mirror set follow their master; only move masters.
    if (CGDisplayMirrorsDisplay(list[i]) != kCGNullDirectDisplay) {
      continue;
    }
    const CGRect other = CGDisplayBounds(list[i]);
    CGConfigureDisplayOrigin(config, list[i], (int32_t) other.origin.x - dx, (int32_t) other.origin.y - dy);
  }
  const CGError err = CGCompleteDisplayConfiguration(config, kCGConfigureForSession);
  log_line(@"made display %u the main display (result %d, main is now %u)", display_id, err, CGMainDisplayID());
}

static void send_ready_if_possible(BOOL mode_ok) {
  if (g_ready_sent) {
    return;
  }
  if (!display_is_active(g_display.displayID)) {
    return;
  }
  // Wait a little for the exact mode, but never block the stream forever on it.
  if (!mode_ok && g_mode_attempts < 15) {
    return;
  }

  if (g_make_primary) {
    make_main_display();
  }

  size_t pixel_wide = 0;
  size_t pixel_high = 0;
  CGDisplayModeRef current = CGDisplayCopyDisplayMode(g_display.displayID);
  if (current) {
    pixel_wide = CGDisplayModeGetPixelWidth(current);
    pixel_high = CGDisplayModeGetPixelHeight(current);
    CGDisplayModeRelease(current);
  }

  printf("READY %u %zu %zu\n", g_display.displayID, pixel_wide, pixel_high);
  fflush(stdout);
  g_ready_sent = YES;
  log_line(@"ready: display %u, %lux%lu pixels", g_display.displayID, (unsigned long) pixel_wide, (unsigned long) pixel_high);
}

static void tick(void) {
  if (!g_display) {
    return;
  }
  ensure_not_mirrored();
  const BOOL mode_ok = select_target_mode();
  if (!mode_ok && g_mode_attempts < 15) {
    ++g_mode_attempts;
  }
  send_ready_if_possible(mode_ok);
  if (g_ready_sent && g_make_primary) {
    // macOS may restore a saved arrangement after a reconfiguration.
    make_main_display();
  }
}

#pragma mark - Entry point

static void usage(void) {
  fprintf(stderr,
          "usage: sunshine_vd_helper --width W --height H [--fps F] [--hidpi 0|1]\n"
          "                          [--primary 0|1] [--serial N] [--name NAME]\n");
}

int main(int argc, const char *argv[]) {
  @autoreleasepool {
    unsigned long width = 0;
    unsigned long height = 0;
    unsigned long fps = 60;
    unsigned long hidpi = 1;
    unsigned long primary = 1;
    unsigned long serial = 1;
    NSString *name = @"MacShine Virtual Display";

    for (int i = 1; i + 1 < argc; i += 2) {
      const char *key = argv[i];
      const char *value = argv[i + 1];
      if (strcmp(key, "--width") == 0) {
        width = parse_ulong(value, 0);
      } else if (strcmp(key, "--height") == 0) {
        height = parse_ulong(value, 0);
      } else if (strcmp(key, "--fps") == 0) {
        fps = parse_ulong(value, 60);
      } else if (strcmp(key, "--hidpi") == 0) {
        hidpi = parse_ulong(value, 1);
      } else if (strcmp(key, "--primary") == 0) {
        primary = parse_ulong(value, 1);
      } else if (strcmp(key, "--serial") == 0) {
        serial = parse_ulong(value, 1);
      } else if (strcmp(key, "--name") == 0) {
        name = [NSString stringWithUTF8String:value] ?: name;
      }
    }

    if (width < 320 || height < 200 || width > 8192 || height > 8192) {
      usage();
      return 2;
    }
    if (fps < 10 || fps > 240) {
      fps = 60;
    }

    // HiDPI needs an even pixel size: the display is W/2 x H/2 points backed by W x H pixels.
    g_scale = (hidpi && width % 2 == 0 && height % 2 == 0) ? 2 : 1;
    g_points_wide = width / g_scale;
    g_points_high = height / g_scale;
    g_make_primary = primary != 0;

    // Physical size drives the reported PPI. CGVirtualDisplay rejects displays that are too dense
    // for their declared size, so derive it from a plausible PPI for the chosen scale.
    const double ppi = g_scale == 2 ? 220.0 : 110.0;
    const CGSize size_mm = CGSizeMake(width / ppi * 25.4, height / ppi * 25.4);
    const unsigned int max_pixels = (unsigned int) (((width > height ? width : height) + 1) & ~1UL);

    CGVirtualDisplayDescriptor *descriptor = [[CGVirtualDisplayDescriptor alloc] init];
    [descriptor setDispatchQueue:dispatch_get_main_queue()];
    descriptor.name = name;
    descriptor.maxPixelsWide = max_pixels;
    descriptor.maxPixelsHigh = max_pixels;
    descriptor.sizeInMillimeters = size_mm;
    // Vendor/product/serial is how macOS remembers a display's settings and arrangement.
    // A stable serial per Moonlight client gives each client its own remembered display.
    descriptor.vendorID = 0x5355;  // "SU"
    descriptor.productID = 0x4E56;  // "NV"
    descriptor.serialNum = (unsigned int) serial;
    descriptor.terminationHandler = ^(__unused id sender, __unused CGVirtualDisplay *display) {
      log_line(@"virtual display terminated by the system");
      exit(3);
    };

    g_display = [[CGVirtualDisplay alloc] initWithDescriptor:descriptor];
    if (!g_display || g_display.displayID == kCGNullDirectDisplay) {
      log_line(@"failed to create virtual display");
      return 1;
    }

    g_settings = [[CGVirtualDisplaySettings alloc] init];
    g_settings.hiDPI = g_scale == 2 ? 1 : 0;
    g_settings.modes = @[[[CGVirtualDisplayMode alloc] initWithWidth:g_points_wide
                                                              height:g_points_high
                                                         refreshRate:(CGFloat) fps]];
    if (![g_display applySettings:g_settings]) {
      // Some macOS versions refuse refresh rates above 60 Hz for virtual displays.
      log_line(@"applySettings failed at %lu Hz, retrying at 60 Hz", fps);
      g_settings.modes = @[[[CGVirtualDisplayMode alloc] initWithWidth:g_points_wide
                                                                height:g_points_high
                                                           refreshRate:60]];
      if (![g_display applySettings:g_settings]) {
        log_line(@"applySettings failed");
        return 1;
      }
    }
    log_line(@"created display %u: %lux%lu points @%lux, %lu Hz, serial %lu",
             g_display.displayID, (unsigned long) g_points_wide, (unsigned long) g_points_high, (unsigned long) g_scale, fps, serial);

    // Lifeline: Sunshine holds the write end of our stdin. EOF means it is gone.
    signal(SIGPIPE, SIG_IGN);
    dispatch_source_t stdin_source =
      dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, STDIN_FILENO, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(stdin_source, ^{
      char buffer[64];
      const ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));
      if (n <= 0) {
        log_line(@"stdin closed, removing display");
        exit(0);
      }
    });
    dispatch_resume(stdin_source);

    signal(SIGTERM, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    dispatch_source_t term_source =
      dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(term_source, ^{
      log_line(@"SIGTERM, removing display");
      exit(0);
    });
    dispatch_resume(term_source);
    dispatch_source_t int_source =
      dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(int_source, ^{
      exit(0);
    });
    dispatch_resume(int_source);

    // Fast ticks until the display is ready, then slow enforcement.
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), 200 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
    __block BOOL slowed = NO;
    dispatch_source_set_event_handler(timer, ^{
      tick();
      if (g_ready_sent && !slowed) {
        slowed = YES;
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), 2 * NSEC_PER_SEC, 200 * NSEC_PER_MSEC);
      }
    });
    dispatch_resume(timer);

    dispatch_main();
  }
}
