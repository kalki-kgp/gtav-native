#include "wgsl.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace wgsl {

namespace {

bool IsWord(char c) { return std::isalnum((unsigned char)c) || c == '_'; }
size_t SkipSpace(const std::string& s, size_t i) {
  while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
  return i;
}
bool Expect(const std::string& s, size_t& i, const char* lit) {
  size_t n = std::strlen(lit);
  if (s.compare(i, n, lit) != 0) return false;
  i += n;
  return true;
}
bool ReadInt(const std::string& s, size_t& i, int& out) {
  size_t b = i;
  int v = 0;
  while (i < s.size() && std::isdigit((unsigned char)s[i])) v = v * 10 + (s[i++] - '0');
  if (i == b) return false;
  if (i < s.size() && s[i] == 'u') i++;
  out = v;
  return true;
}
std::string ReadWord(const std::string& s, size_t& i) {
  size_t b = i;
  while (i < s.size() && IsWord(s[i])) i++;
  return s.substr(b, i - b);
}
std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) a++;
  while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}
std::string StripSpace(const std::string& s) {
  std::string r;
  for (char c : s) if (!std::isspace((unsigned char)c)) r += c;
  return r;
}

// `@location(N)` at i: returns the location and leaves i behind the closing parenthesis.
bool ReadLocation(const std::string& s, size_t& i, int& loc) {
  size_t j = i;
  if (!Expect(s, j, "@location(") || !ReadInt(s, j, loc) || !Expect(s, j, ")")) return false;
  i = j;
  return true;
}
void LocationsIn(const std::string& body, std::vector<int>& out) {
  for (size_t i = body.find("@location("); i != std::string::npos; i = body.find("@location(", i + 1)) {
    size_t j = i;
    int loc;
    if (ReadLocation(body, j, loc)) out.push_back(loc);
  }
}
// The body between the braces of `struct <name> {`, or false.
bool StructBody(const std::string& text, const std::string& name, std::string& body) {
  const std::string key = "struct";
  for (size_t i = text.find(key); i != std::string::npos; i = text.find(key, i + 1)) {
    size_t j = SkipSpace(text, i + key.size());
    if (j == i + key.size()) continue;
    size_t w = j;
    if (ReadWord(text, w) != name) continue;
    w = SkipSpace(text, w);
    if (w >= text.size() || text[w] != '{') continue;
    size_t e = text.find('}', w);
    if (e == std::string::npos) return false;
    body = text.substr(w + 1, e - w - 1);
    return true;
  }
  return false;
}

struct Decl {
  size_t begin = 0, end = 0;  // the whole declaration, through the semicolon
  int group = 0, binding = 0;
  std::string space, name, type;
};
// `@group(G) @binding(B) var<space> name : type;` starting at i.
bool ReadDecl(const std::string& s, size_t i, Decl& d) {
  d.begin = i;
  if (!Expect(s, i, "@group(") || !ReadInt(s, i, d.group) || !Expect(s, i, ")")) return false;
  i = SkipSpace(s, i);
  if (!Expect(s, i, "@binding(") || !ReadInt(s, i, d.binding) || !Expect(s, i, ")")) return false;
  i = SkipSpace(s, i);
  if (!Expect(s, i, "var")) return false;
  d.space.clear();
  if (i < s.size() && s[i] == '<') {
    size_t e = s.find('>', i);
    if (e == std::string::npos) return false;
    d.space = StripSpace(s.substr(i + 1, e - i - 1));
    i = e + 1;
  }
  size_t j = SkipSpace(s, i);
  if (j == i) return false;
  d.name = ReadWord(s, j);
  if (d.name.empty()) return false;
  j = SkipSpace(s, j);
  if (j >= s.size() || s[j] != ':') return false;
  size_t e = s.find(';', j);
  if (e == std::string::npos) return false;
  d.type = Trim(s.substr(j + 1, e - j - 1));
  d.end = e + 1;
  return true;
}

wgpu::TextureViewDimension DimByName(const std::string& n) {
  if (n == "2d_array") return wgpu::TextureViewDimension::e2DArray;
  if (n == "3d") return wgpu::TextureViewDimension::e3D;
  if (n == "cube") return wgpu::TextureViewDimension::Cube;
  if (n == "cube_array") return wgpu::TextureViewDimension::CubeArray;
  if (n == "1d") return wgpu::TextureViewDimension::e1D;
  return wgpu::TextureViewDimension::e2D;
}

}  // namespace

wgpu::TextureFormat FormatByName(const std::string& n) {
  static const struct { const char* name; wgpu::TextureFormat f; } k[] = {
    { "rgba8unorm", wgpu::TextureFormat::RGBA8Unorm }, { "rgba8snorm", wgpu::TextureFormat::RGBA8Snorm },
    { "rgba8uint", wgpu::TextureFormat::RGBA8Uint }, { "rgba8sint", wgpu::TextureFormat::RGBA8Sint },
    { "bgra8unorm", wgpu::TextureFormat::BGRA8Unorm }, { "rgba16float", wgpu::TextureFormat::RGBA16Float },
    { "rgba16uint", wgpu::TextureFormat::RGBA16Uint }, { "rgba16sint", wgpu::TextureFormat::RGBA16Sint },
    { "rgba16unorm", wgpu::TextureFormat::RGBA16Unorm }, { "r32float", wgpu::TextureFormat::R32Float },
    { "r32uint", wgpu::TextureFormat::R32Uint }, { "r32sint", wgpu::TextureFormat::R32Sint },
    { "rg32float", wgpu::TextureFormat::RG32Float }, { "rg32uint", wgpu::TextureFormat::RG32Uint },
    { "rgba32float", wgpu::TextureFormat::RGBA32Float }, { "rgba32uint", wgpu::TextureFormat::RGBA32Uint },
    { "rgba32sint", wgpu::TextureFormat::RGBA32Sint }, { "r16float", wgpu::TextureFormat::R16Float },
    { "rg16float", wgpu::TextureFormat::RG16Float }, { "r8unorm", wgpu::TextureFormat::R8Unorm },
    { "rg8unorm", wgpu::TextureFormat::RG8Unorm }, { "rg11b10ufloat", wgpu::TextureFormat::RG11B10Ufloat },
    { "rgb10a2unorm", wgpu::TextureFormat::RGB10A2Unorm },
  };
  for (const auto& e : k) if (n == e.name) return e.f;
  return wgpu::TextureFormat::Undefined;
}

std::vector<Binding> ParseBindings(const std::string& text) {
  std::vector<Binding> out;
  for (size_t i = text.find("@group("); i != std::string::npos; i = text.find("@group(", i + 1)) {
    Decl d;
    if (!ReadDecl(text, i, d)) continue;
    Binding b;
    b.group = d.group;
    b.binding = d.binding;
    if (d.space == "uniform") b.kind = Kind::Uniform;
    else if (d.space.compare(0, 7, "storage") == 0) b.kind = d.space.find("read_write") != std::string::npos ? Kind::Storage : Kind::ReadOnlyStorage;
    else if (d.type == "sampler") b.kind = Kind::Sampler;
    else if (d.type == "sampler_comparison") b.kind = Kind::SamplerComparison;
    else {
      b.kind = Kind::Texture;
      std::string name = "2d", arg = "f32";
      if (d.type.compare(0, 8, "texture_") == 0) {
        size_t lt = d.type.find('<');
        name = d.type.substr(8, lt == std::string::npos ? std::string::npos : lt - 8);
        if (lt != std::string::npos) {
          size_t gt = d.type.rfind('>');
          arg = d.type.substr(lt + 1, gt == std::string::npos ? std::string::npos : gt - lt - 1);
        }
      }
      if (name.compare(0, 8, "storage_") == 0) {
        b.kind = Kind::StorageTexture;
        b.dim = DimByName(name.substr(8));
        b.formatName = Trim(arg.substr(0, arg.find(',')));
        b.format = FormatByName(b.formatName);
        b.access = arg.find("read_write") != std::string::npos ? wgpu::StorageTextureAccess::ReadWrite
                 : arg.find("read") != std::string::npos ? wgpu::StorageTextureAccess::ReadOnly : wgpu::StorageTextureAccess::WriteOnly;
      } else if (name.compare(0, 6, "depth_") == 0) {
        b.dim = DimByName(name.substr(6));
        b.sample = wgpu::TextureSampleType::Depth;
      } else if (name.compare(0, 13, "multisampled_") == 0) {
        b.multisampled = true;
        b.sample = arg == "u32" ? wgpu::TextureSampleType::Uint : arg == "i32" ? wgpu::TextureSampleType::Sint : wgpu::TextureSampleType::UnfilterableFloat;
      } else {
        b.dim = DimByName(name);
        b.sample = arg == "u32" ? wgpu::TextureSampleType::Uint : arg == "i32" ? wgpu::TextureSampleType::Sint : wgpu::TextureSampleType::Float;
      }
    }
    out.push_back(b);
  }
  return out;
}

std::vector<int> FragmentOutputs(const std::string& text) {
  std::vector<int> out;
  size_t i = text.find("@fragment");
  if (i == std::string::npos) return out;
  i = SkipSpace(text, i + 9);
  if (!Expect(text, i, "fn")) return out;
  i = SkipSpace(text, i);
  if (!Expect(text, i, "main")) return out;
  i = SkipSpace(text, i);
  if (i >= text.size() || text[i] != '(') return out;
  int depth = 0;
  for (; i < text.size(); i++) {
    if (text[i] == '(') depth++;
    else if (text[i] == ')' && --depth == 0) { i++; break; }
  }
  i = SkipSpace(text, i);
  if (!Expect(text, i, "->")) return out;
  size_t brace = text.find('{', i);
  if (brace == std::string::npos) return out;
  std::string ret = text.substr(i, brace - i);
  size_t at = ret.find("@location(");
  int loc;
  if (at != std::string::npos) {
    if (ReadLocation(ret, at, loc)) out.push_back(loc);
    return out;
  }
  std::string body;
  if (StructBody(text, Trim(ret), body)) LocationsIn(body, out);
  return out;
}

std::string PackUniforms(const std::string& text, Pack& pack) {
  pack = Pack();
  std::string out;
  size_t last = 0;
  for (size_t i = text.find("@group("); i != std::string::npos; i = text.find("@group(", i + 1)) {
    Decl d;
    if (!ReadDecl(text, i, d) || d.space != "uniform" || d.binding == 15) continue;
    pack.group = d.group;
    PackMember m;
    m.binding = d.binding;
    m.name = d.name;
    m.type = d.type;
    std::string body;
    if (StructBody(text, d.type, body)) {
      // `array<vec4<f32>, 12u>`: the element count follows the element type
      size_t a = body.find("array<");
      size_t gt = a == std::string::npos ? a : body.find('>', a);
      if (gt != std::string::npos) {
        size_t j = gt + 1;
        if (j < body.size() && body[j] == ',') {
          j = SkipSpace(body, j + 1);
          int count;
          if (ReadInt(body, j, count) && j < body.size() && body[j] == '>') m.size = (uint32_t)count * 16;
        }
      }
    }
    pack.members.push_back(m);
    out.append(text, last, d.begin - last);
    last = d.end;
    i = d.end - 1;
  }
  if (pack.members.empty()) return text;
  out.append(text, last, std::string::npos);
  std::sort(pack.members.begin(), pack.members.end(), [](const PackMember& a, const PackMember& b) { return a.binding < b.binding; });
  uint32_t off = 0;
  for (PackMember& m : pack.members) { m.offset = off; off += m.size; }
  pack.size = off;
  for (const PackMember& m : pack.members) {
    std::string r;
    size_t from = 0;
    for (size_t p = out.find(m.name); p != std::string::npos; p = out.find(m.name, p + m.name.size())) {
      bool left = p == 0 || !IsWord(out[p - 1]), right = p + m.name.size() >= out.size() || !IsWord(out[p + m.name.size()]);
      if (!left || !right) continue;
      r.append(out, from, p - from);
      r += "cbpack_v." + m.name;
      from = p + m.name.size();
    }
    r.append(out, from, std::string::npos);
    out.swap(r);
  }
  std::string st = "struct cbpack_t {\n";
  for (size_t k = 0; k < pack.members.size(); k++) st += "  " + pack.members[k].name + " : " + pack.members[k].type + (k + 1 < pack.members.size() ? ",\n" : ",");
  st += "\n}\n@group(" + std::to_string(pack.group) + "u) @binding(0u) var<uniform> cbpack_v : cbpack_t;\n";
  // the struct must follow the member type definitions: before the first function or entry point
  size_t at = std::string::npos;
  for (size_t p = out.find('\n'); p != std::string::npos; p = out.find('\n', p + 1)) {
    size_t j = p + 1;
    if (out.compare(j, 2, "fn") == 0 && j + 2 < out.size() && std::isspace((unsigned char)out[j + 2])) { at = p; break; }
    if (j < out.size() && out[j] == '@') {
      size_t w = j + 1;
      if (!ReadWord(out, w).empty() && w < out.size() && std::isspace((unsigned char)out[w])) { at = p; break; }
    }
  }
  if (at == std::string::npos) return out + st;
  return out.substr(0, at) + "\n" + st + out.substr(at);
}

std::vector<int> FlatLocations(const std::string& text) {
  std::vector<int> out;
  for (size_t i = text.find("@location("); i != std::string::npos; i = text.find("@location(", i + 1)) {
    size_t j = i;
    int loc;
    if (!ReadLocation(text, j, loc)) continue;
    j = SkipSpace(text, j);
    if (text.compare(j, 17, "@interpolate(flat") == 0) out.push_back(loc);
  }
  return out;
}

std::string MarkFlat(const std::string& text, const std::vector<int>& flat) {
  std::string out;
  size_t last = 0;
  for (size_t i = text.find("@location("); i != std::string::npos; i = text.find("@location(", i + 1)) {
    size_t j = i + 10;
    int loc = 0;
    size_t b = j;
    while (j < text.size() && std::isdigit((unsigned char)text[j])) loc = loc * 10 + (text[j++] - '0');
    if (j == b || text.compare(j, 2, "u)") != 0) continue;
    j += 2;
    // a struct member: the name and its colon follow on the next line
    size_t w = j;
    bool newline = false;
    while (w < text.size() && std::isspace((unsigned char)text[w])) newline |= text[w++] == '\n';
    if (!newline || ReadWord(text, w).empty()) continue;
    w = SkipSpace(text, w);
    if (w >= text.size() || text[w] != ':') continue;
    if (std::find(flat.begin(), flat.end(), loc) == flat.end()) continue;
    out.append(text, last, j - last);
    out += " @interpolate(flat)";
    last = j;
  }
  out.append(text, last, std::string::npos);
  return out;
}

bool AddAlphaTest(const std::string& text, std::string& out) {
  // target 0 must be the usual four-component colour (a few shaders write a scalar there)
  if (text.find("var<private> o0 : vec4<f32>;") == std::string::npos) return false;
  for (size_t i = text.find("main_inner("); i != std::string::npos; i = text.find("main_inner(", i + 1)) {
    // a call statement on its own line (the definition is `fn main_inner(`)
    size_t b = i;
    while (b > 0 && (text[b - 1] == ' ' || text[b - 1] == '\t')) b--;
    if (b == 0 || text[b - 1] != '\n') continue;
    size_t semi = text.find(';', i);
    if (semi == std::string::npos) return false;
    out = text.substr(0, semi + 1) + "\n  if (o0.w < 0.5f) { discard; }" + text.substr(semi + 1);
    return true;
  }
  return false;
}

std::string ReplaceStorageFormat(const std::string& text, int binding, const std::string& from, const std::string& to) {
  for (size_t i = text.find("@binding("); i != std::string::npos; i = text.find("@binding(", i + 1)) {
    size_t j = i + 9;
    int b;
    if (!ReadInt(text, j, b) || b != binding) continue;
    size_t semi = text.find(';', j), t = text.find("texture_storage_", j);
    if (t == std::string::npos || semi == std::string::npos || t > semi) continue;
    size_t lt = text.find('<', t);
    if (lt == std::string::npos || lt > semi || text.compare(lt + 1, from.size(), from) != 0) continue;
    return text.substr(0, lt + 1) + to + text.substr(lt + 1 + from.size());
  }
  return text;
}

bool HasWord(const std::string& text, const char* word) {
  size_t n = std::strlen(word);
  for (size_t p = text.find(word); p != std::string::npos; p = text.find(word, p + 1))
    if ((p == 0 || !IsWord(text[p - 1])) && (p + n >= text.size() || !IsWord(text[p + n]))) return true;
  return false;
}

}  // namespace wgsl
