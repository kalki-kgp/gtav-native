// The game window on macOS: an NSWindow whose content view is backed by the CAMetalLayer the renderer presents into.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include "gpu_host.h"

static CAMetalLayer* gLayer;
static BOOL gWanted;
static dispatch_semaphore_t gReady;

static dispatch_semaphore_t ready(void) {
  static dispatch_once_t once;
  dispatch_once(&once, ^{ gReady = dispatch_semaphore_create(0); });
  return gReady;
}

// ---- input: AppKit events -> host/input.c ----------------------------------------------------------------------------------

static BOOL gCaptured;

// macOS virtual key code -> Windows virtual-key code
static int vkOf(unsigned short k) {
  static const struct { unsigned short mac; int vk; } map[] = {
    { 0x00, 'A' }, { 0x01, 'S' }, { 0x02, 'D' }, { 0x03, 'F' }, { 0x04, 'H' }, { 0x05, 'G' }, { 0x06, 'Z' }, { 0x07, 'X' }, { 0x08, 'C' }, { 0x09, 'V' },
    { 0x0B, 'B' }, { 0x0C, 'Q' }, { 0x0D, 'W' }, { 0x0E, 'E' }, { 0x0F, 'R' }, { 0x10, 'Y' }, { 0x11, 'T' }, { 0x1F, 'O' }, { 0x20, 'U' }, { 0x22, 'I' },
    { 0x23, 'P' }, { 0x25, 'L' }, { 0x26, 'J' }, { 0x28, 'K' }, { 0x2D, 'N' }, { 0x2E, 'M' },
    { 0x12, '1' }, { 0x13, '2' }, { 0x14, '3' }, { 0x15, '4' }, { 0x17, '5' }, { 0x16, '6' }, { 0x1A, '7' }, { 0x1C, '8' }, { 0x19, '9' }, { 0x1D, '0' },
    { 0x18, 0xBB }, { 0x1B, 0xBD }, { 0x1E, 0xDD }, { 0x21, 0xDB }, { 0x27, 0xDE }, { 0x29, 0xBA }, { 0x2A, 0xDC }, { 0x2B, 0xBC }, { 0x2C, 0xBF },
    { 0x2F, 0xBE }, { 0x32, 0xC0 }, { 0x0A, 0xE2 },
    { 0x24, 0x0D }, { 0x30, 0x09 }, { 0x31, 0x20 }, { 0x33, 0x08 }, { 0x35, 0x1B }, { 0x39, 0x14 }, { 0x75, 0x2E }, { 0x72, 0x2D },
    { 0x73, 0x24 }, { 0x77, 0x23 }, { 0x74, 0x21 }, { 0x79, 0x22 }, { 0x7B, 0x25 }, { 0x7E, 0x26 }, { 0x7C, 0x27 }, { 0x7D, 0x28 },
    { 0x38, 0xA0 }, { 0x3C, 0xA1 }, { 0x3B, 0xA2 }, { 0x3E, 0xA3 }, { 0x3A, 0xA4 }, { 0x3D, 0xA5 }, { 0x37, 0x5B }, { 0x36, 0x5C },
    { 0x7A, 0x70 }, { 0x78, 0x71 }, { 0x63, 0x72 }, { 0x76, 0x73 }, { 0x60, 0x74 }, { 0x61, 0x75 }, { 0x62, 0x76 }, { 0x64, 0x77 }, { 0x65, 0x78 },
    { 0x6D, 0x79 }, { 0x67, 0x7A }, { 0x6F, 0x7B },
    { 0x52, 0x60 }, { 0x53, 0x61 }, { 0x54, 0x62 }, { 0x55, 0x63 }, { 0x56, 0x64 }, { 0x57, 0x65 }, { 0x58, 0x66 }, { 0x59, 0x67 }, { 0x5B, 0x68 }, { 0x5C, 0x69 },
    { 0x43, 0x6A }, { 0x45, 0x6B }, { 0x4E, 0x6D }, { 0x41, 0x6E }, { 0x4B, 0x6F }, { 0x4C, 0xFD },
  };
  for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (map[i].mac == k) return map[i].vk;
  return 0;
}
// Aircraft pitch and roll are on the numpad in the game, which this keyboard lacks: I/K, J/L and U/O press those keys too (as the page does).
static int flyAlias(int vk) {
  switch (vk) { case 'I': return 0x68; case 'K': return 0x62; case 'J': return 0x64; case 'L': return 0x66; case 'U': return 0x67; case 'O': return 0x69; }
  return 0;
}
// While captured the cursor is hidden and pinned, and the game gets relative motion (the page's pointer lock).
static void capture(BOOL on) {
  if (gCaptured == on) return;
  gCaptured = on;
  CGAssociateMouseAndMouseCursorPosition(!on);
  if (on) [NSCursor hide]; else [NSCursor unhide];
  host_input_locked(on);
}

@interface GameView : NSView
@end
@implementation GameView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)e { return YES; }
- (void)keyDown:(NSEvent*)e {
  if (e.modifierFlags & NSEventModifierFlagCommand) { [super keyDown:e]; return; }
  int vk = vkOf(e.keyCode);
  host_input_key(vk, 1);
  host_input_key(flyAlias(vk), 1);
  if (vk == 0x0D) host_input_text(13);
  else if (vk == 0x08) host_input_text(8);
  else if (e.characters.length == 1 && [e.characters characterAtIndex:0] >= 0x20 && [e.characters characterAtIndex:0] < 0xF700) host_input_text([e.characters characterAtIndex:0]);
}
- (void)keyUp:(NSEvent*)e {
  int vk = vkOf(e.keyCode);
  host_input_key(vk, 0);
  host_input_key(flyAlias(vk), 0);
}
- (void)flagsChanged:(NSEvent*)e {
  static const struct { unsigned short mac; NSEventModifierFlags flag; } mods[] = {
    { 0x38, NSEventModifierFlagShift }, { 0x3C, NSEventModifierFlagShift }, { 0x3B, NSEventModifierFlagControl }, { 0x3E, NSEventModifierFlagControl },
    { 0x3A, NSEventModifierFlagOption }, { 0x3D, NSEventModifierFlagOption }, { 0x39, NSEventModifierFlagCapsLock },
  };
  for (size_t i = 0; i < sizeof mods / sizeof *mods; i++)
    if (mods[i].mac == e.keyCode) {
      // both keys of a pair share the flag: when it clears, both are up
      BOOL down = (e.modifierFlags & mods[i].flag) != 0;
      if (down) host_input_key(vkOf(e.keyCode), 1);
      else for (size_t j = 0; j < sizeof mods / sizeof *mods; j++) if (mods[j].flag == mods[i].flag) host_input_key(vkOf(mods[j].mac), 0);
    }
}
- (void)button:(int)bit down:(BOOL)down {
  if (down) capture(YES);
  host_input_button(bit, down);
}
- (void)mouseDown:(NSEvent*)e { [self button:0 down:YES]; }
- (void)mouseUp:(NSEvent*)e { [self button:0 down:NO]; }
- (void)rightMouseDown:(NSEvent*)e { [self button:1 down:YES]; }
- (void)rightMouseUp:(NSEvent*)e { [self button:1 down:NO]; }
- (void)otherMouseDown:(NSEvent*)e { [self button:2 down:YES]; }
- (void)otherMouseUp:(NSEvent*)e { [self button:2 down:NO]; }
- (void)moved:(NSEvent*)e {
  if (gCaptured) { host_input_mouse_delta((int)lround(e.deltaX), (int)lround(e.deltaY)); return; }
  NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
  NSSize size = self.bounds.size;
  if (size.width <= 0 || size.height <= 0) return;
  host_input_mouse_at((int)(p.x * host_input_width() / size.width), (int)((size.height - p.y) * host_input_height() / size.height));
}
- (void)mouseMoved:(NSEvent*)e { [self moved:e]; }
- (void)mouseDragged:(NSEvent*)e { [self moved:e]; }
- (void)rightMouseDragged:(NSEvent*)e { [self moved:e]; }
- (void)otherMouseDragged:(NSEvent*)e { [self moved:e]; }
- (void)scrollWheel:(NSEvent*)e {
  // one notch per frame is consumed by the engine; trackpads report fractions
  static double accum;
  accum += e.hasPreciseScrollingDeltas ? e.scrollingDeltaY / 40.0 : e.scrollingDeltaY;
  int n = (int)accum;
  if (n) { accum -= n; host_input_wheel(n); }
}
@end

@interface GameAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end
@implementation GameAppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app { return YES; }
- (void)windowDidBecomeKey:(NSNotification*)n { host_input_focus(1); }
- (void)windowDidResignKey:(NSNotification*)n { capture(NO); host_input_focus(0); }
@end

void host_window_want(int wanted) { gWanted = wanted != 0; }

void* host_window_layer(void) {
  if (!gWanted) return NULL;
  dispatch_semaphore_wait(ready(), DISPATCH_TIME_FOREVER);
  dispatch_semaphore_signal(ready());
  return (__bridge void*)gLayer;
}

void host_window_run(int width, int height) {
  @autoreleasepool {
    NSApplication* app = [NSApplication sharedApplication];
    static GameAppDelegate* delegate;
    delegate = [GameAppDelegate new];
    app.delegate = delegate;
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSMenu* bar = [NSMenu new];
    NSMenuItem* appItem = [NSMenuItem new];
    [bar addItem:appItem];
    NSMenu* appMenu = [NSMenu new];
    [appMenu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    app.mainMenu = bar;

    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, width, height)
                                                   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.title = @"GTA V";
    window.contentAspectRatio = NSMakeSize(width, height);
    gLayer = [CAMetalLayer layer];
    gLayer.contentsGravity = kCAGravityResizeAspect;
    gLayer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    GameView* view = [[GameView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    view.wantsLayer = YES;
    view.layer = gLayer;
    window.contentView = view;
    window.delegate = delegate;
    window.acceptsMouseMovedEvents = YES;
    [window makeFirstResponder:view];
    [window center];
    [window makeKeyAndOrderFront:nil];
    [app activateIgnoringOtherApps:YES];
    dispatch_semaphore_signal(ready());
    [app run];
  }
}
