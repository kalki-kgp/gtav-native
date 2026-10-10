// Runs the host's WGSL rewriting (uniform packing, alpha-test and flat variants) over every translated shader and compiles
// the results with Dawn. usage: wgslcheck <shader folder>
#include <dirent.h>
#include <dawn/webgpu_cpp.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../host/wgsl.h"

static wgpu::Instance instance;
static wgpu::Device device;
static size_t failed;

static bool Compile(const std::string& text, const std::string& what) {
  wgpu::ShaderSourceWGSL src;
  src.code = wgpu::StringView(text.data(), text.size());
  wgpu::ShaderModuleDescriptor smd;
  smd.nextInChain = &src;
  bool ok = true;
  device.PushErrorScope(wgpu::ErrorFilter::Validation);
  wgpu::ShaderModule module = device.CreateShaderModule(&smd);
  instance.WaitAny(device.PopErrorScope(wgpu::CallbackMode::WaitAnyOnly, [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView msg) {
    if (type == wgpu::ErrorType::NoError) return;
    ok = false;
    if (failed++ < 10) printf("REJECTED %s: %.*s\n", what.c_str(), (int)(msg.length > 400 ? 400 : msg.length), msg.data);
  }), UINT64_MAX);
  return ok;
}

int main(int argc, char** argv) {
  static const wgpu::InstanceFeatureName kFeatures[] = { wgpu::InstanceFeatureName::TimedWaitAny };
  wgpu::InstanceDescriptor idesc;
  idesc.requiredFeatureCount = 1;
  idesc.requiredFeatures = kFeatures;
  instance = wgpu::CreateInstance(&idesc);
  wgpu::Adapter adapter;
  instance.WaitAny(instance.RequestAdapter(nullptr, wgpu::CallbackMode::WaitAnyOnly,
                     [&](wgpu::RequestAdapterStatus, wgpu::Adapter a, wgpu::StringView) { adapter = std::move(a); }), UINT64_MAX);
  const wgpu::FeatureName feats[] = { wgpu::FeatureName::ClipDistances, wgpu::FeatureName::DualSourceBlending, wgpu::FeatureName::Float32Filterable,
                                      wgpu::FeatureName::TextureFormatsTier1, wgpu::FeatureName::BGRA8UnormStorage };
  wgpu::DeviceDescriptor ddesc;
  ddesc.requiredFeatureCount = sizeof feats / sizeof *feats;
  ddesc.requiredFeatures = feats;
  device = adapter.CreateDevice(&ddesc);

  std::vector<std::string> files;
  DIR* d = opendir(argv[1]);
  while (dirent* e = readdir(d)) {
    std::string n = e->d_name;
    if (n.size() > 5 && n.compare(n.size() - 5, 5, ".wgsl") == 0) files.push_back(n);
  }
  closedir(d);
  size_t packed = 0, a2c = 0, flatPs = 0, decls = 0, parsed = 0, outputs = 0, unknownFormat = 0;
  for (const std::string& name : files) {
    std::ifstream f(std::string(argv[1]) + "/" + name);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    for (size_t p = text.find("@group("); p != std::string::npos; p = text.find("@group(", p + 1)) decls++;
    wgsl::Pack pack;
    std::string out = wgsl::PackUniforms(text, pack);
    std::vector<wgsl::Binding> bs = wgsl::ParseBindings(out);
    parsed += bs.size() + (pack.members.empty() ? 0 : pack.members.size() - 1);
    for (const wgsl::Binding& b : bs) if (b.kind == wgsl::Kind::StorageTexture && b.format == wgpu::TextureFormat::Undefined) unknownFormat++;
    if (!pack.members.empty()) packed++;
    Compile(out, name + " (packed)");
    if (text.find("@fragment") != std::string::npos) {
      outputs += wgsl::FragmentOutputs(out).size();
      std::string t2;
      if (wgsl::AddAlphaTest(out, t2) && wgsl::FragmentOutputs(out).size() && wgsl::FragmentOutputs(out)[0] == 0) { a2c++; Compile(t2, name + " (alpha test)"); }
      if (!wgsl::FlatLocations(out).empty()) flatPs++;
    }
    if (text.find("@vertex") != std::string::npos) {
      std::string t3 = wgsl::MarkFlat(out, { 1, 2, 3 });
      if (t3 != out) Compile(t3, name + " (flat 1,2,3)");
    }
  }
  printf("%zu shaders: %zu with packed constants, %zu alpha-test variants, %zu pixel shaders with flat inputs, %zu fragment outputs\n", files.size(), packed, a2c, flatPs, outputs);
  printf("declarations %zu, bindings accounted for %zu, storage textures with an unknown format %zu, rejected %zu\n", decls, parsed, unknownFormat, failed);
  return failed ? 1 : 0;
}
