/**
 * @file src/platform/macos/vd_helper.m
 * @brief Standalone helper that owns one CGVirtualDisplay for the lifetime of a stream.
 *
 * Sunshine spawns this helper when a Moonlight session starts and the macOS virtual display
 * option is enabled. The helper creates a virtual monitor that matches the client's requested
 * resolution and refresh rate, optionally makes it the main display or turns every other display
 * off, prints `READY <display id> <pixel width> <pixel height>` on stdout and then keeps the
 * display alive.
 *
 * The display disappears when the helper exits. The helper exits when:
 *   - stdin reaches EOF (Sunshine closed the pipe or died), or
 *   - it receives SIGTERM/SIGINT.
 *
 * Displays the helper turned off are turned back on before it exits. They are also recorded in a
 * state file, so `sunshine_vd_helper --restore` can turn them back on after a crash.
 *
 * The display handling (HiDPI mode enforcement, mirror-set detaching) follows the approach used
 * by OpenDisplay (https://github.com/peetzweg/opendisplay, GPL-3.0). CGVirtualDisplay is a private
 * CoreGraphics API and may break across macOS versions.
 *
 * Compiled with ARC.
 */
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include <dlfcn.h>
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

/** SkyLight's display enable switch, as used by display managers to disconnect a screen. */
typedef CGError (*configure_display_enabled_fn)(CGDisplayConfigRef config, CGDirectDisplayID display, bool enabled);
/** DisplayServices brightness accessors, as used by the brightness keys. */
typedef int (*get_brightness_fn)(CGDirectDisplayID display, float *brightness);
typedef int (*set_brightness_fn)(CGDirectDisplayID display, float brightness);

#pragma mark - State

static CGVirtualDisplay *g_display;
static CGVirtualDisplaySettings *g_settings;
static NSUInteger g_points_wide;
static NSUInteger g_points_high;
static NSUInteger g_scale;
static BOOL g_make_primary;
static BOOL g_disable_others;
static BOOL g_others_disabled;
static BOOL g_others_mirrored;
static BOOL g_ready_sent;
static int g_mode_attempts;
// Every display reconfiguration makes macOS fade the screens. Enforcement after the display is
// ready must therefore be rare and bounded, or the Mac looks frozen and capture never settles.
static int g_mirror_fixes;
static int g_mode_fixes;
static int g_primary_fixes;
static const int MAX_FIXES_AFTER_READY = 2;

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
  // Host displays mirroring the virtual one on purpose (see mirror_other_displays) are fine.
  if (g_others_mirrored && CGDisplayMirrorsDisplay(display_id) == kCGNullDirectDisplay) {
    return;
  }
  if (g_ready_sent && g_mirror_fixes++ >= MAX_FIXES_AFTER_READY) {
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

  // Before ready: at most a few mode switches, spaced out, so the screen does not keep fading.
  static int pre_ready_applies = 0;
  if (!active && !g_ready_sent && (pre_ready_applies >= 3 || g_mode_attempts % 4 != 0)) {
    CFRelease(modes);
    return NO;
  }
  if (!active && !g_ready_sent) {
    ++pre_ready_applies;
  }

  if (!active && g_ready_sent && g_mode_fixes++ >= MAX_FIXES_AFTER_READY) {
    CFRelease(modes);
    return NO;
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
  if (g_ready_sent && g_primary_fixes++ >= MAX_FIXES_AFTER_READY) {
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

#pragma mark - Host displays

/** Displays this helper turned off, to turn back on at exit. */
static CGDirectDisplayID g_disabled_displays[16];
static uint32_t g_disabled_count;
/** Built-in display dimmed to black, with its previous brightness. */
static CGDirectDisplayID g_dimmed_display;
static float g_dimmed_brightness = -1;

/** @return Path of the file that records displays turned off by a helper. */
static NSString *disabled_displays_path(void) {
  NSString *base = [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES) firstObject];
  return [[base stringByAppendingPathComponent:@"MacShine"] stringByAppendingPathComponent:@"disabled-displays"];
}

/** @return The private enable switch, or NULL when this macOS version does not have it. */
static configure_display_enabled_fn display_enabled_switch(void) {
  static configure_display_enabled_fn fn = NULL;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    fn = (configure_display_enabled_fn) dlsym(RTLD_DEFAULT, "CGSConfigureDisplayEnabled");
    if (!fn) {
      void *skylight = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_LAZY);
      fn = skylight ? (configure_display_enabled_fn) dlsym(skylight, "SLSConfigureDisplayEnabled") : NULL;
    }
  });
  return fn;
}

/** Write the displays turned off so far to the state file, or remove it when there are none. */
static void save_disabled_displays(void) {
  NSString *path = disabled_displays_path();
  if (g_disabled_count == 0 && g_dimmed_display == kCGNullDirectDisplay) {
    [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    return;
  }
  // One display id per line; a dimmed display adds "brightness <id> <previous value>".
  NSMutableString *contents = [NSMutableString string];
  for (uint32_t i = 0; i < g_disabled_count; ++i) {
    [contents appendFormat:@"%u\n", g_disabled_displays[i]];
  }
  if (g_dimmed_display != kCGNullDirectDisplay) {
    [contents appendFormat:@"brightness %u %.4f\n", g_dimmed_display, g_dimmed_brightness];
  }
  [[NSFileManager defaultManager] createDirectoryAtPath:[path stringByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:nil];
  [contents writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

/**
 * Look up a DisplayServices function.
 * @param name Symbol name.
 * @return The function, or NULL when unavailable.
 */
static void *display_services_symbol(const char *name) {
  static void *framework = NULL;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    framework = dlopen("/System/Library/PrivateFrameworks/DisplayServices.framework/DisplayServices", RTLD_LAZY);
  });
  return framework ? dlsym(framework, name) : NULL;
}

/**
 * Set the brightness of a display.
 * @return YES on success.
 */
static BOOL set_brightness(CGDirectDisplayID display, float brightness) {
  const set_brightness_fn set = (set_brightness_fn) display_services_symbol("DisplayServicesSetBrightness");
  return set && set(display, brightness) == 0;
}

/**
 * Map every pixel of a display to black. On mini-LED panels black zones turn their backlight off.
 * WindowServer drops the table when this process exits, even after a crash.
 * @param display Display to blacken.
 */
static void blacken_display(CGDirectDisplayID display) {
  static BOOL logged = NO;
  const CGError err = CGSetDisplayTransferByFormula(display, 0, 0, 1, 0, 0, 1, 0, 0, 1);
  if (!logged || err != kCGErrorSuccess) {
    log_line(@"blackened display %u (result %d)", display, err);
    logged = YES;
  }
}

/**
 * Black out the built-in panel. macOS keeps it running as a mirror of the main display when it is
 * "turned off", so its backlight is the only way to make it dark. Call while it is still active.
 * @param displays Candidate displays; the first built-in one is dimmed.
 * @param count Number of candidates.
 */
static void dim_builtin_display(const CGDirectDisplayID *displays, uint32_t count) {
  const get_brightness_fn get = (get_brightness_fn) display_services_symbol("DisplayServicesGetBrightness");
  for (uint32_t i = 0; i < count; ++i) {
    float brightness = -1;
    if (!CGDisplayIsBuiltin(displays[i]) || !get || get(displays[i], &brightness) != 0) {
      continue;
    }
    // Record first, so --restore can bring the backlight back if the helper dies.
    g_dimmed_display = displays[i];
    g_dimmed_brightness = brightness;
    save_disabled_displays();
    const BOOL ok = set_brightness(displays[i], 0);
    log_line(@"dimmed built-in display %u (was %.2f, %s)", displays[i], brightness, ok ? "ok" : "failed");
    blacken_display(displays[i]);
    return;
  }
  log_line(@"no built-in display to dim");
}

/** Give the built-in panel its previous brightness back. */
static void undim_builtin_display(void) {
  if (g_dimmed_display == kCGNullDirectDisplay) {
    return;
  }
  CGDisplayRestoreColorSyncSettings();
  const BOOL ok = set_brightness(g_dimmed_display, g_dimmed_brightness);
  log_line(@"restored brightness %.2f on display %u (%s)", g_dimmed_brightness, g_dimmed_display, ok ? "ok" : "failed");
  if (ok) {
    g_dimmed_display = kCGNullDirectDisplay;
  }
}

/**
 * Turn the given displays on or off in one session-scoped configuration.
 * @return The configuration result.
 */
static CGError set_displays_enabled(const CGDirectDisplayID *displays, uint32_t count, bool enabled) {
  const configure_display_enabled_fn configure_enabled = display_enabled_switch();
  if (!configure_enabled || count == 0) {
    return kCGErrorFailure;
  }
  CGDisplayConfigRef config = NULL;
  CGError err = CGBeginDisplayConfiguration(&config);
  if (err != kCGErrorSuccess) {
    return err;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const CGError display_err = configure_enabled(config, displays[i], enabled);
    if (display_err != kCGErrorSuccess) {
      log_line(@"display %u: enable=%d rejected (%d)", displays[i], enabled, display_err);
    }
  }
  // Session scope: logging out also brings every display back.
  err = CGCompleteDisplayConfiguration(config, kCGConfigureForSession);
  return err;
}

/** Log the active displays, so the effect of a configuration is visible. */
static void log_active_displays(void) {
  uint32_t count = 0;
  CGGetActiveDisplayList(0, NULL, &count);
  CGDirectDisplayID list[count > 0 ? count : 1];
  CGGetActiveDisplayList(count, list, &count);
  NSMutableString *ids = [NSMutableString string];
  for (uint32_t i = 0; i < count; ++i) {
    [ids appendFormat:@"%s%u%s", i ? ", " : "", list[i], CGDisplayIsBuiltin(list[i]) ? " (built-in)" : ""];
  }
  log_line(@"active displays: %@, main %u", ids, CGMainDisplayID());
}

/** Forget recorded displays that are active again, whoever turned them back on. */
static void drop_active_displays(void) {
  uint32_t kept = 0;
  for (uint32_t i = 0; i < g_disabled_count; ++i) {
    if (!display_is_active(g_disabled_displays[i])) {
      g_disabled_displays[kept++] = g_disabled_displays[i];
    }
  }
  g_disabled_count = kept;
}

/**
 * Collect the active displays other than the virtual one.
 * @param targets Receives up to 16 display ids.
 * @return Number of displays written.
 */
static uint32_t other_active_displays(CGDirectDisplayID targets[16]) {
  uint32_t count = 0;
  CGGetActiveDisplayList(0, NULL, &count);
  if (count == 0) {
    return 0;
  }
  CGDirectDisplayID list[count];
  CGGetActiveDisplayList(count, list, &count);

  uint32_t target_count = 0;
  for (uint32_t i = 0; i < count && target_count < 16; ++i) {
    if (list[i] != g_display.displayID) {
      targets[target_count++] = list[i];
    }
  }
  return target_count;
}

/**
 * Fallback when macOS keeps a display on: let it mirror the virtual display, so the virtual display
 * still holds the only desktop. The mirror ends by itself when the virtual display goes away.
 */
static void mirror_other_displays(const CGDirectDisplayID *displays, uint32_t count) {
  CGDisplayConfigRef config = NULL;
  if (CGBeginDisplayConfiguration(&config) != kCGErrorSuccess) {
    return;
  }
  g_others_mirrored = YES;
  for (uint32_t i = 0; i < count; ++i) {
    CGConfigureDisplayMirrorOfDisplay(config, displays[i], g_display.displayID);
  }
  const CGError err = CGCompleteDisplayConfiguration(config, kCGConfigureForSession);
  log_line(@"mirroring %u host display(s) to the virtual display instead (result %d)", count, err);
}

/** Turn every active display except the virtual one off, so the stream gets the whole desktop. */
static void disable_other_displays(void) {
  if (g_others_disabled) {
    return;
  }
  g_others_disabled = YES;

  // macOS is more willing to turn a display off once it is no longer the main display.
  make_main_display();

  CGDirectDisplayID targets[16];
  uint32_t target_count = other_active_displays(targets);
  if (target_count == 0) {
    return;
  }
  dim_builtin_display(targets, target_count);

  if (display_enabled_switch()) {
    // Record first: if the helper dies right after the switch, --restore still knows what to undo.
    memcpy(g_disabled_displays, targets, sizeof(CGDirectDisplayID) * target_count);
    g_disabled_count = target_count;
    save_disabled_displays();

    // WindowServer applies the change asynchronously and sometimes ignores the first request.
    for (int attempt = 0; attempt < 3 && target_count > 0; ++attempt) {
      const CGError err = set_displays_enabled(targets, target_count, false);
      for (int wait = 0; wait < 10; ++wait) {
        usleep(100 * 1000);
        if ((target_count = other_active_displays(targets)) == 0) {
          break;
        }
      }
      log_line(@"turn host displays off, attempt %d: result %d, %u still on", attempt + 1, err, target_count);
    }
    drop_active_displays();
    save_disabled_displays();
  } else {
    log_line(@"turning host displays off is not supported on this macOS version");
  }

  if (target_count > 0) {
    mirror_other_displays(targets, target_count);
  }
  // The reconfiguration may reset the backlight and color table; apply both again.
  if (g_dimmed_display != kCGNullDirectDisplay) {
    set_brightness(g_dimmed_display, 0);
    blacken_display(g_dimmed_display);
  }
  log_active_displays();
}

/** Turn back on the displays this helper turned off. Registered with atexit. */
static void restore_other_displays(void) {
  undim_builtin_display();
  drop_active_displays();
  if (g_disabled_count == 0) {
    save_disabled_displays();
    return;
  }
  const uint32_t requested = g_disabled_count;
  const CGError err = set_displays_enabled(g_disabled_displays, g_disabled_count, true);
  // macOS may report an error and still bring the display back, so check the result instead.
  drop_active_displays();
  save_disabled_displays();
  log_line(@"turned %u of %u host display(s) back on (result %d)", requested - g_disabled_count, requested, err);
}

/**
 * Turn back on displays recorded by a helper that did not exit cleanly.
 * @return Process exit code.
 */
static int restore_from_state_file(void) {
  NSString *contents = [NSString stringWithContentsOfFile:disabled_displays_path() encoding:NSUTF8StringEncoding error:nil];
  if (!contents) {
    return 0;
  }
  g_disabled_count = 0;
  for (NSString *line in [contents componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]]) {
    NSArray<NSString *> *fields = [line componentsSeparatedByString:@" "];
    if (fields.count == 3 && [fields[0] isEqualToString:@"brightness"]) {
      g_dimmed_display = (CGDirectDisplayID) parse_ulong(fields[1].UTF8String, 0);
      g_dimmed_brightness = fields[2].floatValue;
      continue;
    }
    const unsigned long id = parse_ulong(line.UTF8String, 0);
    if (id != 0 && g_disabled_count < 16) {
      g_disabled_displays[g_disabled_count++] = (CGDirectDisplayID) id;
    }
  }
  restore_other_displays();
  return g_disabled_count == 0 && g_dimmed_display == kCGNullDirectDisplay ? 0 : 1;
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

  if (g_disable_others) {
    // With every other display off, the virtual display is the only one and becomes main.
    disable_other_displays();
  } else if (g_make_primary) {
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
  if (g_ready_sent && g_dimmed_display != kCGNullDirectDisplay) {
    blacken_display(g_dimmed_display);
  }
  if (g_ready_sent && g_make_primary && !g_disable_others) {
    // macOS may restore a saved arrangement after a reconfiguration.
    make_main_display();
  }
}

#pragma mark - Entry point

static void usage(void) {
  fprintf(stderr,
          "usage: sunshine_vd_helper --width W --height H [--fps F] [--hidpi 0|1]\n"
          "                          [--primary 0|1] [--disable-others 0|1] [--serial N] [--name NAME]\n"
          "       sunshine_vd_helper --restore\n");
}

int main(int argc, const char *argv[]) {
  @autoreleasepool {
    if (argc == 2 && strcmp(argv[1], "--restore") == 0) {
      return restore_from_state_file();
    }

    unsigned long width = 0;
    unsigned long height = 0;
    unsigned long fps = 60;
    unsigned long hidpi = 1;
    unsigned long primary = 1;
    unsigned long disable_others = 0;
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
      } else if (strcmp(key, "--disable-others") == 0) {
        disable_others = parse_ulong(value, 0);
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
    g_disable_others = disable_others != 0;

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

    // Every exit path below goes through exit(), which runs this before the display goes away.
    atexit(restore_other_displays);

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
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), 5 * NSEC_PER_SEC, 500 * NSEC_PER_MSEC);
      }
    });
    dispatch_resume(timer);

    dispatch_main();
  }
}
