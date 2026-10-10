// Compiles every translated WGSL shader of the browser build with Dawn's Metal backend and reports the ones it rejects.
// usage: shadercheck <shader folder>
#include <dirent.h>
#include <webgpu/webgpu_cpp.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <shader folder>\n", argv[0]); return 2; }
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
  if (!adapter) { fprintf(stderr, "no adapter\n"); return 1; }
  wgpu::AdapterInfo info;
  adapter.GetInfo(&info);
  printf("adapter: %.*s (%.*s)\n", (int)info.device.length, info.device.data, (int)info.description.length, info.description.data);

  // the features the browser renderer asks for (wgpu_worker.js initDevice)
  const wgpu::FeatureName want[] = { wgpu::FeatureName::TextureCompressionBC, wgpu::FeatureName::TextureCompressionBCSliced3D,
    wgpu::FeatureName::Depth32FloatStencil8, wgpu::FeatureName::Float32Filterable, wgpu::FeatureName::IndirectFirstInstance,
    wgpu::FeatureName::RG11B10UfloatRenderable, wgpu::FeatureName::BGRA8UnormStorage, wgpu::FeatureName::DepthClipControl,
    wgpu::FeatureName::ClipDistances, wgpu::FeatureName::TextureFormatsTier1, wgpu::FeatureName::DualSourceBlending };
  const char* names[] = { "texture-compression-bc", "texture-compression-bc-sliced-3d", "depth32float-stencil8", "float32-filterable",
    "indirect-first-instance", "rg11b10ufloat-renderable", "bgra8unorm-storage", "depth-clip-control", "clip-distances",
    "texture-formats-tier1", "dual-source-blending" };
  std::vector<wgpu::FeatureName> have;
  for (size_t i = 0; i < sizeof want / sizeof *want; i++) {
    bool ok = adapter.HasFeature(want[i]);
    printf("  feature %-34s %s\n", names[i], ok ? "yes" : "NO");
    if (ok) have.push_back(want[i]);
  }
  wgpu::DeviceDescriptor ddesc;
  ddesc.requiredFeatureCount = have.size();
  ddesc.requiredFeatures = have.data();
  wgpu::Device device = adapter.CreateDevice(&ddesc);
  if (!device) { fprintf(stderr, "no device\n"); return 1; }

  std::vector<std::string> files;
  DIR* d = opendir(argv[1]);
  if (!d) { perror(argv[1]); return 1; }
  while (dirent* e = readdir(d)) {
    std::string n = e->d_name;
    if (n.size() > 5 && n.compare(n.size() - 5, 5, ".wgsl") == 0) files.push_back(n);
  }
  closedir(d);
  size_t limit = argc > 2 ? (size_t)atoi(argv[2]) : files.size();
  if (limit < files.size()) files.resize(limit);

  auto t0 = std::chrono::steady_clock::now();
  size_t failed = 0;
  for (const std::string& name : files) {
    std::ifstream f(std::string(argv[1]) + "/" + name);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    wgpu::ShaderSourceWGSL wgsl;
    wgsl.code = wgpu::StringView(text.data(), text.size());
    wgpu::ShaderModuleDescriptor smd;
    smd.nextInChain = &wgsl;
    device.PushErrorScope(wgpu::ErrorFilter::Validation);
    wgpu::ShaderModule module = device.CreateShaderModule(&smd);
    instance.WaitAny(device.PopErrorScope(wgpu::CallbackMode::WaitAnyOnly,
                       [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView msg) {
                         if (type == wgpu::ErrorType::NoError) return;
                         if (failed++ < 12) printf("REJECTED %s: %.*s\n", name.c_str(), (int)(msg.length > 300 ? 300 : msg.length), msg.data);
                       }), UINT64_MAX);
  }
  double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  printf("%zu shaders, %zu rejected, %.1f s\n", files.size(), failed, s);
  return failed ? 1 : 0;
}
