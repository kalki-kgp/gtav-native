// The iPhone app: a Metal-backed game view, touch controls that feed the engine's keyboard-and-mouse input block, and startup.
// The engine, renderer and audio are in libgta.a (host/).
#import <AVFAudio/AVFAudio.h>
#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>
#include <os/proc.h>

#include "gpu_host.h"

// Windows virtual-key codes the controls press
enum { VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D, VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28,
       VK_LSHIFT = 0xA0, VK_LCONTROL = 0xA2 };

// ---- game view ----------------------------------------------------------------------------------------------------------

@interface GameView : UIView
@end
@implementation GameView
+ (Class)layerClass { return CAMetalLayer.class; }
@end

// ---- touch controls -------------------------------------------------------------------------------------------------------
// The game is played with keyboard and mouse. Left side: a floating stick that holds W/A/S/D (and Shift when pushed far).
// Anywhere else: dragging moves the mouse (camera). Buttons hold a key or a mouse button.

typedef NS_ENUM(int, Act) { ActKey, ActMouse, ActToggleMouse, ActWheel };
typedef struct { const char* label; Act act; int code; CGFloat x, y; /* centre, in units of the view height, x from the right edge when negative */ CGFloat r; } ButtonSpec;

static const ButtonSpec kButtons[] = {
  // right hand
  { "FIRE", ActMouse, 0, -0.20, 0.72, 0.105 },
  { "AIM", ActToggleMouse, 1, -0.42, 0.84, 0.075 },
  { "JUMP", ActKey, VK_SPACE, -0.40, 0.60, 0.075 },
  { "ENTER", ActKey, 'F', -0.22, 0.44, 0.070 },
  { "RELOAD", ActKey, 'R', -0.07, 0.50, 0.060 },
  { "COVER", ActKey, 'Q', -0.07, 0.30, 0.060 },
  { "WEAPON", ActWheel, 1, -0.38, 0.36, 0.060 },
  // top row
  { "PAUSE", ActKey, 'P', 0.10, 0.09, 0.055 },
  { "PHONE", ActKey, VK_UP, 0.24, 0.09, 0.055 },
  { "CAM", ActKey, 'V', 0.38, 0.09, 0.055 },
  { "ESC", ActKey, VK_ESCAPE, -0.10, 0.09, 0.055 },
  // menu keys
  { "▲", ActKey, VK_UP, 0.80, 0.09, 0.050 },
  { "▼", ActKey, VK_DOWN, 0.92, 0.09, 0.050 },
  { "◀", ActKey, VK_LEFT, 1.04, 0.09, 0.050 },
  { "▶", ActKey, VK_RIGHT, 1.16, 0.09, 0.050 },
  { "OK", ActKey, VK_RETURN, 1.28, 0.09, 0.050 },
  { "BACK", ActKey, VK_BACK, 1.40, 0.09, 0.050 },
};
enum { kButtonCount = sizeof kButtons / sizeof *kButtons };

@interface ControlsView : UIView
@end
@implementation ControlsView {
  UILabel* _labels[kButtonCount];
  UITouch* _buttonTouch[kButtonCount];
  BOOL _toggled[kButtonCount];
  UITouch* _stickTouch;
  CGPoint _stickOrigin;
  UIView* _stickBase;
  UIView* _stickKnob;
  BOOL _held[5];  // W A S D Shift
}

- (instancetype)initWithFrame:(CGRect)frame {
  if ((self = [super initWithFrame:frame])) {
    self.multipleTouchEnabled = YES;
    self.backgroundColor = UIColor.clearColor;
    for (int i = 0; i < kButtonCount; i++) {
      UILabel* l = [UILabel new];
      l.text = [NSString stringWithUTF8String:kButtons[i].label];
      l.textAlignment = NSTextAlignmentCenter;
      l.textColor = [UIColor colorWithWhite:1 alpha:0.75];
      l.font = [UIFont systemFontOfSize:10 weight:UIFontWeightSemibold];
      l.adjustsFontSizeToFitWidth = YES;
      l.backgroundColor = [UIColor colorWithWhite:1 alpha:0.14];
      l.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.35].CGColor;
      l.layer.borderWidth = 1;
      l.layer.masksToBounds = YES;
      [self addSubview:l];
      _labels[i] = l;
    }
    _stickBase = [UIView new];
    _stickBase.backgroundColor = [UIColor colorWithWhite:1 alpha:0.10];
    _stickBase.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.30].CGColor;
    _stickBase.layer.borderWidth = 1;
    _stickBase.hidden = YES;
    _stickKnob = [UIView new];
    _stickKnob.backgroundColor = [UIColor colorWithWhite:1 alpha:0.30];
    _stickKnob.hidden = YES;
    [self addSubview:_stickBase];
    [self addSubview:_stickKnob];
  }
  return self;
}

- (CGRect)frameOfButton:(int)i {
  CGFloat h = self.bounds.size.height, w = self.bounds.size.width;
  const ButtonSpec* b = &kButtons[i];
  CGFloat cx = b->x < 0 ? w + b->x * h : b->x * h, cy = b->y * h, r = b->r * h;
  return CGRectMake(cx - r, cy - r, 2 * r, 2 * r);
}

- (void)layoutSubviews {
  [super layoutSubviews];
  for (int i = 0; i < kButtonCount; i++) {
    _labels[i].frame = [self frameOfButton:i];
    _labels[i].layer.cornerRadius = _labels[i].frame.size.width / 2;
  }
}

- (void)setButton:(int)i down:(BOOL)down {
  const ButtonSpec* b = &kButtons[i];
  switch (b->act) {
    case ActKey: host_input_key(b->code, down); break;
    case ActMouse: host_input_button(b->code, down); break;
    case ActToggleMouse:
      if (!down) return;
      _toggled[i] = !_toggled[i];
      host_input_button(b->code, _toggled[i]);
      _labels[i].backgroundColor = [UIColor colorWithWhite:1 alpha:_toggled[i] ? 0.40 : 0.14];
      return;
    case ActWheel:
      if (down) host_input_wheel(b->code);
      break;
  }
  _labels[i].backgroundColor = [UIColor colorWithWhite:1 alpha:down ? 0.40 : 0.14];
}

- (void)setStick:(CGPoint)v {
  // v in points from the origin; keys by direction, Shift (sprint) when pushed far
  const CGFloat dead = 14, far = 62;
  BOOL want[5] = { v.y < -dead, v.x < -dead, v.y > dead, v.x > dead, hypot(v.x, v.y) > far };
  static const int keys[5] = { 'W', 'A', 'S', 'D', VK_LSHIFT };
  for (int k = 0; k < 5; k++) if (want[k] != _held[k]) { _held[k] = want[k]; host_input_key(keys[k], want[k]); }
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  for (UITouch* t in touches) {
    CGPoint p = [t locationInView:self];
    int hit = -1;
    for (int i = 0; i < kButtonCount && hit < 0; i++) {
      CGRect f = [self frameOfButton:i];
      if (!_buttonTouch[i] && hypot(p.x - CGRectGetMidX(f), p.y - CGRectGetMidY(f)) <= f.size.width / 2 + 6) hit = i;
    }
    if (hit >= 0) { _buttonTouch[hit] = t; [self setButton:hit down:YES]; continue; }
    if (!_stickTouch && p.x < self.bounds.size.width * 0.38 && p.y > self.bounds.size.height * 0.25) {
      _stickTouch = t;
      _stickOrigin = p;
      const CGFloat r = 62;
      _stickBase.frame = CGRectMake(p.x - r, p.y - r, 2 * r, 2 * r);
      _stickBase.layer.cornerRadius = r;
      _stickKnob.frame = CGRectMake(p.x - 24, p.y - 24, 48, 48);
      _stickKnob.layer.cornerRadius = 24;
      _stickBase.hidden = _stickKnob.hidden = NO;
    }
    // any other touch is a look drag: handled in touchesMoved
  }
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  for (UITouch* t in touches) {
    CGPoint p = [t locationInView:self];
    if (t == _stickTouch) {
      CGPoint v = CGPointMake(p.x - _stickOrigin.x, p.y - _stickOrigin.y);
      CGFloat len = hypot(v.x, v.y), max = 62;
      CGPoint knob = len > max ? CGPointMake(_stickOrigin.x + v.x * max / len, _stickOrigin.y + v.y * max / len) : p;
      _stickKnob.center = knob;
      [self setStick:v];
      continue;
    }
    BOOL isButton = NO;
    for (int i = 0; i < kButtonCount; i++) if (_buttonTouch[i] == t && kButtons[i].act != ActMouse) isButton = YES;
    if (isButton) continue;
    // a look drag (also while holding FIRE, so aiming and shooting use one thumb); points -> back-buffer pixels
    CGPoint q = [t previousLocationInView:self];
    CGFloat k = 2.2 * host_input_width() / MAX(1, self.bounds.size.width);
    host_input_mouse_delta((int)lround((p.x - q.x) * k), (int)lround((p.y - q.y) * k));
  }
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  for (UITouch* t in touches) {
    if (t == _stickTouch) {
      _stickTouch = nil;
      _stickBase.hidden = _stickKnob.hidden = YES;
      [self setStick:CGPointZero];
      continue;
    }
    for (int i = 0; i < kButtonCount; i++) if (_buttonTouch[i] == t) { _buttonTouch[i] = nil; [self setButton:i down:NO]; }
  }
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { [self touchesEnded:touches withEvent:event]; }
@end

// ---- view controller ----------------------------------------------------------------------------------------------------------

// ---- game controller ---------------------------------------------------------------------------------------------------
// The engine only reads keyboard and mouse here, so a controller is turned into those: the usual GTA layout, expressed
// through the keyboard defaults. Left stick W/A/S/D (it also drives), right stick the mouse, triggers the mouse buttons.

@interface PadReader : NSObject
@property(weak, nonatomic) UIView* touchControls;  // hidden while a controller is connected
- (void)start;
@end
@implementation PadReader {
  CADisplayLink* _link;
  BOOL _keys[256], _mouse[2], _active;
  CFTimeInterval _last;
  double _fracX, _fracY, _lookSpeed;
}
- (void)start {
  // back-buffer pixels a second at full deflection (GTA_PAD_LOOK in Documents/env.txt changes it)
  _lookSpeed = getenv("GTA_PAD_LOOK") ? atof(getenv("GTA_PAD_LOOK")) : 1.0 * host_input_width();
  _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
  [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}
static double LookCurve(float v) {
  const double dead = 0.12;
  double a = fabs(v);
  if (a < dead) return 0;
  return copysign(pow((a - dead) / (1 - dead), 1.7), v);
}
- (void)tick:(CADisplayLink*)link {
  GCExtendedGamepad* g = nil;
  for (GCController* c in GCController.controllers) if (c.extendedGamepad) { g = c.extendedGamepad; break; }
  BOOL keys[256] = { 0 }, mouse[2] = { 0 };
  double dt = MIN(0.05, _last ? link.timestamp - _last : 0);
  _last = link.timestamp;
  if (g) {
    const float dead = 0.3f;
    float lx = g.leftThumbstick.xAxis.value, ly = g.leftThumbstick.yAxis.value;
    keys['A'] = lx < -dead; keys['D'] = lx > dead; keys['W'] = ly > dead; keys['S'] = ly < -dead;
    mouse[0] = g.rightTrigger.value > 0.2f;                   // fire
    mouse[1] = g.leftTrigger.value > 0.2f;                    // aim
    if (g.buttonA.pressed) keys[VK_LSHIFT] = keys[VK_RETURN] = YES;  // sprint; accept in menus
    if (g.buttonB.pressed) keys['R'] = keys[VK_BACK] = YES;          // reload; back in menus
    keys[VK_SPACE] = g.buttonX.pressed;                       // jump, handbrake
    keys['F'] = g.buttonY.pressed;                            // enter / leave a vehicle
    keys['Q'] = g.rightShoulder.pressed;                      // cover
    keys[VK_TAB] = g.leftShoulder.pressed;                    // weapon wheel
    keys[VK_LCONTROL] = g.leftThumbstickButton.pressed;       // stealth
    keys['C'] = g.rightThumbstickButton.pressed;              // look behind
    keys[VK_UP] = g.dpad.up.pressed; keys[VK_DOWN] = g.dpad.down.pressed;
    keys[VK_LEFT] = g.dpad.left.pressed; keys[VK_RIGHT] = g.dpad.right.pressed;
    keys['P'] = g.buttonMenu.pressed;                         // pause
    keys['V'] = g.buttonOptions.pressed;                      // camera
    _fracX += LookCurve(g.rightThumbstick.xAxis.value) * _lookSpeed * dt;
    _fracY -= LookCurve(g.rightThumbstick.yAxis.value) * _lookSpeed * dt;
    int dx = (int)_fracX, dy = (int)_fracY;
    _fracX -= dx; _fracY -= dy;
    if (dx || dy) host_input_mouse_delta(dx, dy);
  }
  if (g || _active) {  // on disconnect everything held is let go
    for (int k = 0; k < 256; k++) if (keys[k] != _keys[k]) { _keys[k] = keys[k]; host_input_key(k, keys[k]); }
    for (int b = 0; b < 2; b++) if (mouse[b] != _mouse[b]) { _mouse[b] = mouse[b]; host_input_button(b, mouse[b]); }
  }
  if ((g != nil) != _active) {
    _active = g != nil;
    self.touchControls.hidden = _active;
    host_log("controller %s", _active ? "connected: touch controls hidden" : "disconnected");
  }
}
@end

@interface GameViewController : UIViewController
@end
@implementation GameViewController {
  BOOL _started;
  ControlsView* _controls;
  PadReader* _pad;
}
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }

- (void)loadView { self.view = [[GameView alloc] initWithFrame:UIScreen.mainScreen.bounds]; }

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = UIColor.blackColor;
  ControlsView* controls = [[ControlsView alloc] initWithFrame:self.view.bounds];
  controls.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [self.view addSubview:controls];
  _controls = controls;
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  CGSize size = self.view.bounds.size;
  if (_started || size.width <= size.height) return;  // wait for the landscape layout
  _started = YES;
  [self startGameWithViewSize:size];
}

- (void)showMessage:(NSString*)text {
  UILabel* l = [[UILabel alloc] initWithFrame:CGRectInset(self.view.bounds, 60, 40)];
  l.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  l.numberOfLines = 0;
  l.textColor = UIColor.whiteColor;
  l.textAlignment = NSTextAlignmentCenter;
  l.text = text;
  [self.view addSubview:l];
}

- (void)startGameWithViewSize:(CGSize)size {
  NSString* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
  NSString* data = [docs stringByAppendingPathComponent:@"data"], *shaders = [docs stringByAppendingPathComponent:@"shaders"];
  NSString* user = [docs stringByAppendingPathComponent:@"userdata"];
  // the engine's and the host's output: Documents/game.log (readable over USB)
  NSString* log = [docs stringByAppendingPathComponent:@"game.log"];
  freopen(log.fileSystemRepresentation, "w", stderr);
  dup2(fileno(stderr), fileno(stdout));
  setvbuf(stderr, NULL, _IOLBF, 0);
  NSFileManager* fm = NSFileManager.defaultManager;
  if (![fm fileExistsAtPath:[data stringByAppendingPathComponent:@"manifest.json"]] || ![fm fileExistsAtPath:shaders]) {
    [self showMessage:@"Game data not found.\nCopy the data folder to Documents/data and the shaders to Documents/shaders."];
    return;
  }
  [fm createDirectoryAtPath:user withIntermediateDirectories:YES attributes:nil error:nil];

  // Documents/env.txt (optional, test aid): KEY=VALUE lines become environment variables the host reads (GTA_GPU, GTA_AUTOPLAY, ...)
  NSString* env = [NSString stringWithContentsOfFile:[docs stringByAppendingPathComponent:@"env.txt"] encoding:NSUTF8StringEncoding error:nil];
  for (NSString* line in [env componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
    NSRange eq = [line rangeOfString:@"="];
    if (eq.location == NSNotFound || [line hasPrefix:@"#"]) continue;
    if ([line hasPrefix:@"GTA_AUTOPLAY="]) continue;  // the scripted player is for the Mac; on the phone a person plays
    setenv([line substringToIndex:eq.location].UTF8String, [line substringFromIndex:eq.location + 1].UTF8String, 1);
  }

  AVAudioSession* audio = AVAudioSession.sharedInstance;
  [audio setCategory:AVAudioSessionCategoryPlayback error:nil];
  [audio setPreferredSampleRate:48000 error:nil];
  [audio setActive:YES error:nil];
  UIApplication.sharedApplication.idleTimerDisabled = YES;

  // back buffer: the screen's shape at a height the GPU and the memory budget are comfortable with
  // Settings app > GTA V (Settings.bundle): frame rate and picture height
  NSUserDefaults* prefs = NSUserDefaults.standardUserDefaults;
  [prefs registerDefaults:@{ @"fps": @60, @"height": @640 }];
  int fps = (int)[prefs integerForKey:@"fps"];
  int height = (int)MIN(1080, MAX(360, [prefs integerForKey:@"height"])), width = ((int)lround(height * size.width / size.height) + 1) & ~1;
  CAMetalLayer* layer = (CAMetalLayer*)self.view.layer;
  layer.contentsGravity = kCAGravityResizeAspect;
  layer.opaque = YES;
  host_window_set_layer((__bridge void*)layer);

  // the engine sizes its thread pools from the core count; the phone has 2 fast cores and little memory per thread
  setenv("GTA_CORES", "4", 0);
  // the browser page's low-memory profile (index.html LOW_ARGS): a quarter of the texture memory, no shadows, minimum variety
  static char* args[24] = { "-textureQuality=0", "-pedVariety=0", "-vehicleVariety=0", "-shadowQuality=-1", "-reflectionQuality=0", "-particleQuality=0",
                          "-grassQuality=0", "-cityDensity=0", "-lodScale=0", "-pedLodBias=0", "-vehicleLodBias=0",
                          // The engine sizes its streaming budget from the video memory it is told about; the port reports 4 GB, and the
                          // streamer then keeps loading as the player moves until iOS ends the app. With 1 GB it stays level.
                          "-availablevidmem=1024", "-disableExtraMem" };
  int argc = 0;
  while (args[argc]) argc++;
  // the engine waits this many display refreshes per frame: 2 is 30 fps, which roughly halves the work and the heat
  if (fps <= 30) args[argc++] = "-frameLimit=2";

  host_config c = { .data_dir = data.fileSystemRepresentation, .user_dir = user.fileSystemRepresentation, .shader_dir = shaders.fileSystemRepresentation,
                    .gpu_mode = getenv("GTA_GPU") ? getenv("GTA_GPU") : "dawn", .window = 1, .width = width, .height = height, .argc = argc, .argv = args };
  host_log("iPhone start: back buffer %dx%d at %d fps, view %.0fx%.0f pt, %zu MB available to the app", width, height, fps <= 30 ? 30 : 60, size.width, size.height, os_proc_available_memory() >> 20);
  host_start(&c);
  _pad = [PadReader new];
  _pad.touchControls = _controls;
  [_pad start];
  [NSTimer scheduledTimerWithTimeInterval:10 repeats:YES block:^(NSTimer* t) {
    host_log("memory still available to the app: %zu MB", os_proc_available_memory() >> 20);
  }];
}
@end

// ---- app --------------------------------------------------------------------------------------------------------------------

@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(strong, nonatomic) UIWindow* window;
@end
@implementation SceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
  self.window.rootViewController = [GameViewController new];
  [self.window makeKeyAndVisible];
}
- (void)sceneDidBecomeActive:(UIScene*)scene { host_input_focus(1); }
- (void)sceneWillResignActive:(UIScene*)scene { host_input_focus(0); }
- (void)sceneDidEnterBackground:(UIScene*)scene { gpu_suspend(1); }
- (void)sceneWillEnterForeground:(UIScene*)scene { gpu_suspend(0); }
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation AppDelegate
- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)opts { return YES; }
@end

int main(int argc, char** argv) {
  @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class)); }
}
