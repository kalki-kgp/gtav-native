// Device probe: what the phone's GPU and memory allow, before the renderer is ported.
#import <Metal/Metal.h>
#import <UIKit/UIKit.h>
#include <mach/mach.h>
#include <os/proc.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <webgpu/webgpu_cpp.h>

#include <chrono>
#include <string>
#include <vector>

static UITextView* gText;

static void out(NSString* fmt, ...) NS_FORMAT_FUNCTION(1, 2);
static void out(NSString* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  NSString* line = [[NSString alloc] initWithFormat:fmt arguments:ap];
  va_end(ap);
  printf("PROBE %s\n", line.UTF8String);
  fflush(stdout);
  dispatch_async(dispatch_get_main_queue(), ^{
    gText.text = [gText.text stringByAppendingFormat:@"%@\n", line];
  });
}

static uint64_t sysctl_u64(const char* name) {
  uint64_t v = 0;
  size_t n = sizeof v;
  return sysctlbyname(name, &v, &n, NULL, 0) ? 0 : v;
}

static double footprint_mb() {
  task_vm_info_data_t info;
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return -1;
  return info.phys_footprint / 1048576.0;
}

static void probe_system() {
  out(@"system: %llu MB RAM, %llu cores (%llu performance, %llu efficiency), iOS %@", sysctl_u64("hw.memsize") >> 20, sysctl_u64("hw.ncpu"),
      sysctl_u64("hw.perflevel0.physicalcpu"), sysctl_u64("hw.perflevel1.physicalcpu"), UIDevice.currentDevice.systemVersion);
  out(@"memory available to this app at start: %zu MB (footprint %.0f MB)", os_proc_available_memory() >> 20, footprint_mb());
}

static void probe_metal() {
  id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
  out(@"metal: %@, BC texture compression %@, working set %llu MB, max buffer %llu MB", dev.name,
      dev.supportsBCTextureCompression ? @"YES" : @"NO", dev.recommendedMaxWorkingSetSize >> 20, (uint64_t)dev.maxBufferLength >> 20);
}

static void probe_dawn() {
  static const wgpu::InstanceFeatureName kFeatures[] = { wgpu::InstanceFeatureName::TimedWaitAny };
  wgpu::InstanceDescriptor idesc;
  idesc.requiredFeatureCount = 1;
  idesc.requiredFeatures = kFeatures;
  wgpu::Instance instance = wgpu::CreateInstance(&idesc);
  wgpu::Adapter adapter;
  wgpu::RequestAdapterOptions opts;
  opts.powerPreference = wgpu::PowerPreference::HighPerformance;
  instance.WaitAny(instance.RequestAdapter(&opts, wgpu::CallbackMode::WaitAnyOnly,
                     [&](wgpu::RequestAdapterStatus, wgpu::Adapter a, wgpu::StringView) { adapter = std::move(a); }), UINT64_MAX);
  if (!adapter) { out(@"dawn: NO ADAPTER"); return; }
  wgpu::AdapterInfo info;
  adapter.GetInfo(&info);
  out(@"dawn adapter: %.*s (%.*s)", (int)info.device.length, info.device.data, (int)info.description.length, info.description.data);

  // the features the browser renderer asks for (wgpu_worker.js initDevice)
  const wgpu::FeatureName want[] = { wgpu::FeatureName::TextureCompressionBC, wgpu::FeatureName::TextureCompressionBCSliced3D,
    wgpu::FeatureName::Depth32FloatStencil8, wgpu::FeatureName::Float32Filterable, wgpu::FeatureName::IndirectFirstInstance,
    wgpu::FeatureName::RG11B10UfloatRenderable, wgpu::FeatureName::BGRA8UnormStorage, wgpu::FeatureName::DepthClipControl,
    wgpu::FeatureName::ClipDistances, wgpu::FeatureName::TextureFormatsTier1, wgpu::FeatureName::DualSourceBlending,
    wgpu::FeatureName::TextureCompressionASTC, wgpu::FeatureName::TextureCompressionETC2 };
  const char* names[] = { "texture-compression-bc", "texture-compression-bc-sliced-3d", "depth32float-stencil8", "float32-filterable",
    "indirect-first-instance", "rg11b10ufloat-renderable", "bgra8unorm-storage", "depth-clip-control", "clip-distances",
    "texture-formats-tier1", "dual-source-blending", "(extra) texture-compression-astc", "(extra) texture-compression-etc2" };
  std::vector<wgpu::FeatureName> have;
  for (size_t i = 0; i < sizeof want / sizeof *want; i++) {
    bool ok = adapter.HasFeature(want[i]);
    out(@"dawn feature %-34s %s", names[i], ok ? "yes" : "NO");
    if (ok) have.push_back(want[i]);
  }
  wgpu::Limits lim;
  adapter.GetLimits(&lim);
  out(@"dawn limits: texture2D %u, buffer %llu MB, uniform binding %llu, storage binding %llu MB, colour bytes/sample %u, inter-stage vars %u, "
      "sampled textures/stage %u, bind groups %u, dynamic uniform buffers %u, vertex buffers %u",
      lim.maxTextureDimension2D, lim.maxBufferSize >> 20, lim.maxUniformBufferBindingSize, lim.maxStorageBufferBindingSize >> 20,
      lim.maxColorAttachmentBytesPerSample, lim.maxInterStageShaderVariables, lim.maxSampledTexturesPerShaderStage, lim.maxBindGroups,
      lim.maxDynamicUniformBuffersPerPipelineLayout, lim.maxVertexBuffers);

  wgpu::DeviceDescriptor ddesc;
  ddesc.requiredFeatureCount = have.size();
  ddesc.requiredFeatures = have.data();
  ddesc.requiredLimits = &lim;
  wgpu::Device device = adapter.CreateDevice(&ddesc);
  if (!device) { out(@"dawn: NO DEVICE"); return; }
  out(@"dawn device created with the adapter's full limits");

  NSString* dir = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"shaders"];
  NSArray<NSString*>* files = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self ENDSWITH '.wgsl'"]];
  auto t0 = std::chrono::steady_clock::now();
  size_t failed = 0;
  size_t done = 0;
  for (NSString* name in files) {
    NSData* data = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:name]];
    wgpu::ShaderSourceWGSL wgsl;
    wgsl.code = wgpu::StringView((const char*)data.bytes, data.length);
    wgpu::ShaderModuleDescriptor smd;
    smd.nextInChain = &wgsl;
    device.PushErrorScope(wgpu::ErrorFilter::Validation);
    wgpu::ShaderModule module = device.CreateShaderModule(&smd);
    instance.WaitAny(device.PopErrorScope(wgpu::CallbackMode::WaitAnyOnly,
                       [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView msg) {
                         if (type == wgpu::ErrorType::NoError) return;
                         if (failed++ < 8) out(@"shader REJECTED %@: %.*s", name, (int)(msg.length > 240 ? 240 : msg.length), msg.data);
                       }), UINT64_MAX);
    if (++done % 1000 == 0) out(@"shaders: %zu of %lu checked", done, (unsigned long)files.count);
  }
  out(@"shaders: %lu compiled, %zu rejected, %.1f s", (unsigned long)files.count, failed,
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

static void probe_memory() {
  // address space: the engine wants one contiguous heap (3 GB at start in the browser build)
  for (uint64_t gb : { 2ull, 3ull, 4ull, 6ull, 8ull, 16ull }) {
    void* p = mmap(NULL, gb << 30, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    out(@"reserve %llu GB of address space: %s", gb, p == MAP_FAILED ? strerror(errno) : "ok");
    if (p != MAP_FAILED) munmap(p, gb << 30);
  }
  // real memory: touch 64 MB at a time with incompressible bytes and stop well before iOS would kill the app
  const size_t step = 64u << 20;
  uint64_t x = 0x9E3779B97F4A7C15ull;
  size_t total = 0;
  for (;;) {
    size_t avail = os_proc_available_memory();
    if (avail < (400u << 20) || total >= (7000ull << 20)) break;
    uint64_t* p = (uint64_t*)mmap(NULL, step, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { out(@"commit: mmap failed at %zu MB: %s", total >> 20, strerror(errno)); break; }
    for (size_t i = 0; i < step / 8; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; p[i] = x; }
    total += step;
    if ((total >> 20) % 512 == 0) out(@"commit: %zu MB in use, footprint %.0f MB, still available %zu MB", total >> 20, footprint_mb(), os_proc_available_memory() >> 20);
  }
  out(@"commit: stopped at %zu MB in use, footprint %.0f MB, still available %zu MB", total >> 20, footprint_mb(), os_proc_available_memory() >> 20);
}

@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(strong, nonatomic) UIWindow* window;
@end

@implementation SceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
  UIViewController* vc = [UIViewController new];
  gText = [[UITextView alloc] initWithFrame:vc.view.bounds];
  gText.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  gText.editable = NO;
  gText.font = [UIFont monospacedSystemFontOfSize:10 weight:UIFontWeightRegular];
  gText.text = @"";
  [vc.view addSubview:gText];
  self.window.rootViewController = vc;
  [self.window makeKeyAndVisible];
  UIApplication.sharedApplication.idleTimerDisabled = YES;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    probe_system();
    probe_metal();
    probe_dawn();
    probe_memory();
    out(@"DONE");
  });
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)opts { return YES; }
@end

int main(int argc, char** argv) {
  @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class)); }
}
