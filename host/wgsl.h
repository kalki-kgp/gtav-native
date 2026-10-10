// Text-level reflection and rewriting of the translated WGSL shaders (the same steps wgpu_worker.js does with regular
// expressions: the translator emits plain, regular declarations).
#ifndef GTA_WGSL_H
#define GTA_WGSL_H

#include <dawn/webgpu_cpp.h>

#include <string>
#include <vector>

namespace wgsl {

enum class Kind { Uniform, Storage, ReadOnlyStorage, Sampler, SamplerComparison, Texture, StorageTexture };

struct Binding {
  int group = 0, binding = 0;
  Kind kind = Kind::Texture;
  wgpu::TextureViewDimension dim = wgpu::TextureViewDimension::e2D;
  wgpu::TextureSampleType sample = wgpu::TextureSampleType::Float;
  bool multisampled = false;
  std::string formatName;  // storage textures: the texel format as written in the shader
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  wgpu::StorageTextureAccess access = wgpu::StorageTextureAccess::WriteOnly;
};

// One constant buffer of a stage inside the merged uniform struct.
struct PackMember {
  int binding = 0;
  std::string name, type;
  uint32_t size = 16, offset = 0;
};
struct Pack {
  std::vector<PackMember> members;
  uint32_t size = 0;
  int group = 0;
};

wgpu::TextureFormat FormatByName(const std::string& name);
std::vector<Binding> ParseBindings(const std::string& text);
std::vector<int> FragmentOutputs(const std::string& text);
// Merges every `var<uniform>` except the constant table at binding 15 into one struct at binding 0. Returns the new text;
// `pack.members` stays empty when the shader has no constant buffers.
std::string PackUniforms(const std::string& text, Pack& pack);
// Locations the pixel shader reads with @interpolate(flat).
std::vector<int> FlatLocations(const std::string& text);
// The vertex shader with those outputs marked flat.
std::string MarkFlat(const std::string& text, const std::vector<int>& flat);
// Adds the alpha test that stands in for alpha-to-coverage on a single-sample target. False: nothing to patch.
bool AddAlphaTest(const std::string& text, std::string& out);
// Replaces the texel format of the storage texture at `binding`.
std::string ReplaceStorageFormat(const std::string& text, int binding, const std::string& from, const std::string& to);
bool HasWord(const std::string& text, const char* word);

}  // namespace wgsl

#endif
