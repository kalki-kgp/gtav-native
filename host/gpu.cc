// Native renderer: executes the D3D11-style command stream the engine writes into a ring buffer in its heap, on WebGPU
// (Dawn). A port of the browser build's wgpu_worker.js; the ring protocol and the opcodes are described there and in the
// engine's wgpu_backend.h.
#include <dawn/webgpu_cpp.h>

#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gpu_host.h"
#include "wgsl.h"

namespace {

using wgsl::Binding;
using wgsl::Kind;
typedef wgpu::TextureFormat F;

// ---- shared heap ---------------------------------------------------------------------------------------------------------

uint8_t* M;
inline uint32_t* U32(uint64_t word) { return (uint32_t*)(M + word * 4); }
inline float* F32(uint64_t word) { return (float*)(M + word * 4); }
inline int32_t ALoad(uint64_t word) { return __atomic_load_n((int32_t*)(M + word * 4), __ATOMIC_SEQ_CST); }
inline void AStore(uint64_t word, int32_t v) { __atomic_store_n((int32_t*)(M + word * 4), v, __ATOMIC_SEQ_CST); }
inline void AAdd(uint64_t word, int32_t v) { __atomic_fetch_add((int32_t*)(M + word * 4), v, __ATOMIC_SEQ_CST); }
inline void Notify(uint64_t word) { host_futex_notify(word * 4, UINT32_MAX); }

void LogOnce(const std::string& key, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void LogOnce(const std::string& key, const char* fmt, ...) {
  static std::unordered_set<std::string> seen;
  if (!seen.insert(key).second) return;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  host_log("[gpu] %s", buf);
}

struct KeyHash {
  size_t operator()(const std::vector<uint32_t>& k) const {
    uint32_t h = 0x811c9dc5;
    for (uint32_t v : k) h = (h ^ v) * 16777619u;
    return h;
  }
};

// ---- D3D11 (DXGI) -> WebGPU formats ------------------------------------------------------------------------------------------

enum { BIND_VB = 1, BIND_IB = 2, BIND_CB = 4, BIND_SRV = 0x8, BIND_RT = 0x20, BIND_DS = 0x40, BIND_UAV = 0x80 };

struct Fmt {
  F format;
  F view = F::Undefined;  // extra view format (sRGB reinterpretation)
  bool a8 = false;
};

Fmt MapFormat(uint32_t f, uint32_t bind) {
  const bool ds = (bind & BIND_DS) != 0;
  switch (f) {
    case 2: return { F::RGBA32Float };
    case 9: case 10: case 11: return { F::RGBA16Float };  // no core rgba16unorm
    case 16: return { F::RG32Float };
    case 23: case 24: return { F::RGB10A2Unorm };
    case 26: return { F::RG11B10Ufloat };
    case 27: case 28: return { F::RGBA8Unorm, F::RGBA8UnormSrgb };
    case 29: return { F::RGBA8UnormSrgb, F::RGBA8Unorm };
    case 90: case 87: case 88: return { F::BGRA8Unorm, F::BGRA8UnormSrgb };
    case 91: return { F::BGRA8UnormSrgb, F::BGRA8Unorm };
    case 33: case 34: case 35: return { F::RG16Float };
    case 39: return { ds ? F::Depth32Float : F::R32Float };
    case 40: return { F::Depth32Float };
    case 41: return { F::R32Float };
    case 19: case 20: return { F::Depth32FloatStencil8 };
    case 44: case 45: return { F::Depth24PlusStencil8 };
    case 48: case 49: return { F::RG8Unorm };
    case 53: return { ds ? F::Depth16Unorm : F::R16Float };
    case 55: return { F::Depth16Unorm };
    case 54: return { F::R16Float };
    case 60: case 61: return { F::R8Unorm };
    case 63: return { F::R8Snorm };
    case 51: return { F::RG8Snorm };
    case 31: return { F::RGBA8Snorm };
    case 67: return { F::RGB9E5Ufloat };
    case 65: return { F::RGBA8Unorm, F::Undefined, true };  // A8_UNORM: expanded to (0,0,0,a) on upload
    case 56: return { F::Depth16Unorm };
    case 21: case 22: case 46: case 47: return { F::Depth24PlusStencil8 };  // views of a depth texture: the aspect selects the plane
    case 70: case 71: return { F::BC1RGBAUnorm, F::BC1RGBAUnormSrgb };
    case 72: return { F::BC1RGBAUnormSrgb, F::BC1RGBAUnorm };
    case 73: case 74: return { F::BC2RGBAUnorm, F::BC2RGBAUnormSrgb };
    case 75: return { F::BC2RGBAUnormSrgb, F::BC2RGBAUnorm };
    case 76: case 77: return { F::BC3RGBAUnorm, F::BC3RGBAUnormSrgb };
    case 78: return { F::BC3RGBAUnormSrgb, F::BC3RGBAUnorm };
    case 79: case 80: return { F::BC4RUnorm };
    case 82: case 83: return { F::BC5RGUnorm };
    case 94: case 95: return { F::BC6HRGBUfloat };
    case 97: case 98: return { F::BC7RGBAUnorm, F::BC7RGBAUnormSrgb };
    case 99: return { F::BC7RGBAUnormSrgb, F::BC7RGBAUnorm };
  }
  LogOnce("fmt" + std::to_string(f), "no WebGPU format for DXGI %u (using rgba8unorm)", f);
  return { F::RGBA8Unorm };
}

bool HasStencil(F f) { return f == F::Depth24PlusStencil8 || f == F::Depth32FloatStencil8 || f == F::Stencil8; }
bool IsDepth(F f) { return f == F::Depth16Unorm || f == F::Depth24Plus || f == F::Depth32Float || HasStencil(f); }
bool IsBC(F f) { return f >= F::BC1RGBAUnorm && f <= F::BC7RGBAUnormSrgb; }
bool IsSrgb(F f) {
  return f == F::RGBA8UnormSrgb || f == F::BGRA8UnormSrgb || f == F::BC1RGBAUnormSrgb || f == F::BC2RGBAUnormSrgb || f == F::BC3RGBAUnormSrgb || f == F::BC7RGBAUnormSrgb;
}
uint32_t BlockBytes(F f) {
  if (!IsBC(f)) return 0;
  return f == F::BC1RGBAUnorm || f == F::BC1RGBAUnormSrgb || f == F::BC4RUnorm || f == F::BC4RSnorm ? 8 : 16;
}
// bytes per texel of an uncompressed format (0: not one the copy and clear paths handle)
uint32_t TexelBytes(F f) {
  switch (f) {
    case F::RGBA32Float: case F::RGBA32Uint: case F::RGBA32Sint: return 16;
    case F::RGBA16Float: case F::RGBA16Uint: case F::RGBA16Sint: case F::RG32Float: case F::RG32Uint: return 8;
    case F::RGBA8Unorm: case F::RGBA8UnormSrgb: case F::RGBA8Snorm: case F::RGBA8Uint: case F::BGRA8Unorm: case F::BGRA8UnormSrgb:
    case F::R32Float: case F::R32Uint: case F::R32Sint: case F::RG16Float: case F::RGB10A2Unorm: case F::RG11B10Ufloat: case F::RGB9E5Ufloat: return 4;
    case F::RG8Unorm: case F::RG8Snorm: case F::R16Float: return 2;
    case F::R8Unorm: case F::R8Snorm: case F::R8Uint: return 1;
    default: return 0;
  }
}

// ---- D3D11 enums -> WebGPU ------------------------------------------------------------------------------------------------------

wgpu::CompareFunction Cmp(uint32_t v, wgpu::CompareFunction fallback) {
  static const wgpu::CompareFunction k[] = { wgpu::CompareFunction::Undefined, wgpu::CompareFunction::Never, wgpu::CompareFunction::Less,
    wgpu::CompareFunction::Equal, wgpu::CompareFunction::LessEqual, wgpu::CompareFunction::Greater, wgpu::CompareFunction::NotEqual,
    wgpu::CompareFunction::GreaterEqual, wgpu::CompareFunction::Always };
  return v >= 1 && v <= 8 ? k[v] : fallback;
}
wgpu::StencilOperation StencilOp(uint32_t v) {
  static const wgpu::StencilOperation k[] = { wgpu::StencilOperation::Keep, wgpu::StencilOperation::Keep, wgpu::StencilOperation::Zero,
    wgpu::StencilOperation::Replace, wgpu::StencilOperation::IncrementClamp, wgpu::StencilOperation::DecrementClamp,
    wgpu::StencilOperation::Invert, wgpu::StencilOperation::IncrementWrap, wgpu::StencilOperation::DecrementWrap };
  return v <= 8 ? k[v] : wgpu::StencilOperation::Keep;
}
wgpu::BlendFactor BlendFactor(uint32_t v, wgpu::BlendFactor fallback) {
  typedef wgpu::BlendFactor B;
  static const B k[] = { B::Undefined, B::Zero, B::One, B::Src, B::OneMinusSrc, B::SrcAlpha, B::OneMinusSrcAlpha, B::DstAlpha, B::OneMinusDstAlpha,
    B::Dst, B::OneMinusDst, B::SrcAlphaSaturated, B::Undefined, B::Undefined, B::Constant, B::OneMinusConstant, B::Src1, B::OneMinusSrc1,
    B::Src1Alpha, B::OneMinusSrc1Alpha };
  return v < 20 && k[v] != B::Undefined ? k[v] : fallback;
}
wgpu::BlendOperation BlendOp(uint32_t v) {
  typedef wgpu::BlendOperation B;
  static const B k[] = { B::Add, B::Add, B::Subtract, B::ReverseSubtract, B::Min, B::Max };
  return v <= 5 ? k[v] : B::Add;
}
wgpu::AddressMode Address(uint32_t v) {
  typedef wgpu::AddressMode A;
  static const A k[] = { A::Repeat, A::Repeat, A::MirrorRepeat, A::ClampToEdge, A::ClampToEdge, A::MirrorRepeat };
  return v <= 5 ? k[v] : A::Repeat;
}
struct Topology { wgpu::PrimitiveTopology topology; bool strip; };
Topology TopologyOf(uint32_t v) {
  typedef wgpu::PrimitiveTopology P;
  switch (v) {
    case 1: return { P::PointList, false };
    case 2: return { P::LineList, false };
    case 3: return { P::LineStrip, true };
    case 5: return { P::TriangleStrip, true };
  }
  return { P::TriangleList, false };
}
struct VFormat { wgpu::VertexFormat format; uint32_t bytes; };
VFormat VertexFormatOf(uint32_t dxgi) {
  typedef wgpu::VertexFormat V;
  switch (dxgi) {
    case 2: return { V::Float32x4, 16 }; case 6: return { V::Float32x3, 12 }; case 16: return { V::Float32x2, 8 }; case 41: return { V::Float32, 4 };
    case 3: return { V::Uint32x4, 16 }; case 7: return { V::Uint32x3, 12 }; case 17: return { V::Uint32x2, 8 }; case 42: return { V::Uint32, 4 };
    case 4: return { V::Sint32x4, 16 }; case 8: return { V::Sint32x3, 12 }; case 18: return { V::Sint32x2, 8 }; case 43: return { V::Sint32, 4 };
    case 10: return { V::Float16x4, 8 }; case 34: return { V::Float16x2, 4 };
    case 11: return { V::Unorm16x4, 8 }; case 35: return { V::Unorm16x2, 4 };
    case 12: return { V::Uint16x4, 8 }; case 36: return { V::Uint16x2, 4 };
    case 13: return { V::Snorm16x4, 8 }; case 37: return { V::Snorm16x2, 4 };
    case 14: return { V::Sint16x4, 8 }; case 38: return { V::Sint16x2, 4 };
    case 28: return { V::Unorm8x4, 4 }; case 30: return { V::Uint8x4, 4 }; case 31: return { V::Snorm8x4, 4 }; case 32: return { V::Sint8x4, 4 };
    case 49: return { V::Unorm8x2, 2 }; case 50: return { V::Uint8x2, 2 }; case 51: return { V::Snorm8x2, 2 }; case 52: return { V::Sint8x2, 2 };
    case 87: return { V::Unorm8x4BGRA, 4 }; case 24: return { V::Unorm10_10_10_2, 4 };
  }
  return { V::Float32, 0 };  // bytes 0: unsupported
}
inline float WordFloat(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }

// ---- objects ------------------------------------------------------------------------------------------------------------------

wgpu::Instance instance;
wgpu::Adapter adapter;
wgpu::Device device;
wgpu::Queue queue;
wgpu::Surface surface;
wgpu::Limits limits;
bool hasBC = false;
uint64_t ringW = 0;  // word index of the ring header

enum class OK { Texture, Buffer, Shader, Layout, State, Srv, Uav, Query };
struct Obj {
  OK kind;
  uint64_t enc = 0;   // == encEpoch: referenced by commands recorded since the last submit
  bool inBG = false;  // some cached bind group may hold it
  explicit Obj(OK k) : kind(k) {}
  virtual ~Obj() {}
};

struct ProxyPass { wgpu::TextureView dst; wgpu::BindGroup bg; };
struct TexObj : Obj {
  TexObj() : Obj(OK::Texture) {}
  wgpu::Texture tex;
  uint32_t width = 1, height = 1, layers = 1, mips = 1, samples = 1, dim = 2;  // dim 3: `layers` is the depth
  F format = F::Undefined;
  bool a8 = false, renderable = false;
  uint64_t bytes = 0;
  std::unordered_map<uint64_t, wgpu::TextureView> views;
  // depth read as float / float read as depth: copies kept in step by a pass (see DepthProxy)
  std::unique_ptr<TexObj> proxyDepth, proxyStencil, proxyFDepth;
  bool dirty = true;
  std::vector<ProxyPass> passes, mipPasses;
  wgpu::BindGroup blitBG;
};
struct BufObj : Obj {
  BufObj() : Obj(OK::Buffer) {}
  uint32_t id = 0, size = 0, bind = 0, misc = 0, stride = 0, version = 0;
  uint64_t bytes = 0;
  wgpu::Buffer gpu, counter;
  std::vector<uint8_t> shadow;  // constant buffers live here and are copied to the uniform ring per draw
};
struct SigEl { uint32_t sem, idx, reg, mask; };
struct Alloc;
struct PackEnt {
  uint64_t frame = 0;
  std::vector<uint32_t> ids, vers, firsts;
  struct Chunk* chunk = nullptr;
  uint32_t off = 0;
};
struct BglRef { wgpu::BindGroupLayout layout; uint32_t uid = 0; };
struct Program {
  wgpu::ShaderModule module, a2c;
  bool a2cTried = false;
  std::vector<Binding> bindings, groupBindings[2];
  std::vector<int> outputs, flat;
  wgpu::Buffer consts;
  wgsl::Pack pack;
  std::string text, label;
  std::unordered_map<std::string, wgpu::ShaderModule> variants;
  BglRef bgl[3];
  std::vector<PackEnt> packEnts[3];
  uint32_t uid = 0;
  bool vid = false, iid = false, hasGroup[2] = { false, false };
};
struct ShaderObj : Obj {
  ShaderObj() : Obj(OK::Shader) {}
  uint32_t stage = 0, hid = 0, hashLo = 0, hashHi = 0;
  std::string hash;
  std::vector<SigEl> sig;
  bool tried = false;
  Program* prog = nullptr;  // shared by every shader object with this hash; null: no usable WGSL
};
struct LayoutEl { uint32_t sem, idx, fmt, slot, offset, cls, rate; };
struct LayoutSlot { uint32_t slot, cls; };
struct LayoutObj : Obj {
  LayoutObj() : Obj(OK::Layout) {}
  std::vector<LayoutEl> els;
  std::vector<LayoutSlot> slots;
};
struct StateObj : Obj {
  StateObj() : Obj(OK::State) {}
  uint32_t skind = 0;
  std::vector<uint32_t> words;
  wgpu::Sampler sampler;
};
struct ViewObj : Obj {  // SRV or UAV
  explicit ViewObj(OK k) : Obj(k) {}
  uint32_t res = 0, fmt = 0, vdim = 0, a = 0, b = 0, c = 0, d = 0;
  bool isBuf = false;
  struct Cached { wgpu::TextureView view; WGPUTexture source; };
  std::unordered_map<uint32_t, Cached> views;
};
struct QueryObj : Obj {
  QueryObj() : Obj(OK::Query) {}
  uint32_t type = 0, seq = 0;
  uint64_t addr = 0;
  std::vector<uint32_t> slots;
};

std::vector<std::unique_ptr<Obj>> objs;  // ids come from one counter in the engine and are never reused
template <typename T> T* Get(uint32_t id, OK kind) {
  if (!id || id >= objs.size() || !objs[id] || objs[id]->kind != kind) return nullptr;
  return static_cast<T*>(objs[id].get());
}
Obj* GetAny(uint32_t id) { return id && id < objs.size() ? objs[id].get() : nullptr; }
TexObj* Tex(uint32_t id) { return Get<TexObj>(id, OK::Texture); }
BufObj* Buf(uint32_t id) { return Get<BufObj>(id, OK::Buffer); }
void Put(uint32_t id, Obj* o) {
  if (id >= objs.size()) objs.resize(std::max<size_t>(id + 1, objs.size() * 2));
  objs[id].reset(o);
}

// ---- submit thread ------------------------------------------------------------------------------------------------------------
// Dawn records commands when they are issued and translates them to Metal at Submit, which costs about as much again. The
// command thread only records; everything that goes through the queue (writes, submits, and whatever must follow a submit:
// destroys, maps, fences, the present) runs on this second thread, in the order it was issued.

// where each renderer thread is, for the stall watchdog
std::atomic<const char*> cmdStage{ "starting" }, submitStage{ "starting" };
std::atomic<uint32_t> cmdLastOp{ 0 };
std::atomic<bool> suspended{ false };  // the app is in the background: iOS forbids GPU work there
std::mutex qMu;
std::condition_variable qCv, qSpaceCv;
std::deque<std::function<void()>> qOps;
int qSubmitsPending = 0;

void Q(std::function<void()> op) {
  { std::lock_guard<std::mutex> lock(qMu); qOps.push_back(std::move(op)); }
  qCv.notify_one();
}
void QWriteBuffer(const wgpu::Buffer& buffer, uint64_t offset, const void* data, size_t size) {
  std::vector<uint8_t> copy((const uint8_t*)data, (const uint8_t*)data + size);
  Q([buffer, offset, copy = std::move(copy)] { queue.WriteBuffer(buffer, offset, copy.data(), copy.size()); });
}
void QWriteTexture(const wgpu::TexelCopyTextureInfo& dst, std::vector<uint8_t> data, const wgpu::TexelCopyBufferLayout& layout, const wgpu::Extent3D& ext) {
  Q([dst, layout, ext, data = std::move(data)] { queue.WriteTexture(&dst, data.data(), data.size(), &layout, &ext); });
}
void* SubmitMain(void*) {
  pthread_setname_np("gpu submit");
  for (;;) {
    std::deque<std::function<void()>> batch;
    {
      std::unique_lock<std::mutex> lock(qMu);
      submitStage = "waiting for work";
      if (qOps.empty()) qCv.wait_for(lock, std::chrono::milliseconds(2));
      batch.swap(qOps);
    }
    submitStage = "running queued work";
    for (auto& op : batch) op();
    submitStage = "processing completion events";
    instance.ProcessEvents();  // completion callbacks (maps, fences, queries) run here
  }
  return nullptr;
}

// ---- encoder, pass, submit ----------------------------------------------------------------------------------------------------

wgpu::CommandEncoder encoder;
wgpu::RenderPassEncoder pass;
bool hasEncoder = false, hasPass = false;
uint64_t encEpoch = 1;
uint64_t drawCount = 0, skippedDraws = 0, submits = 0, presentCount = 0, errorCount = 0, pipelineCount = 0, bgCreated = 0, dispatchCount = 0;
double pipeMs = 0;
std::map<std::string, uint64_t> skipReasons, flushReasons;
// per-interval statistics (the fps line): render passes are what an Apple (tile-based) GPU pays for most
uint64_t statPasses = 0, statProxyPasses = 0, statClearPasses = 0, statFences = 0, statReadbacks = 0;
double statFenceMs = 0, statSlotMs = 0, statReadbackMs = 0, statPresentMs = 0;

struct PassState {  // what the open pass already has set
  WGPURenderPipeline pl = nullptr;
  WGPUBindGroup bg[2] = { nullptr, nullptr };
  int64_t dyn[2] = { -2, -2 };
  WGPUBuffer vb[16] = {};
  uint64_t vbo[16] = {};
  WGPUBuffer ib = nullptr;
  uint32_t ibFmt = 0;
  int64_t ibOff = -1;
  bool hasVp = false, hasSc = false, hasBf = false;
  float vp[6], bf[4];
  uint32_t sc[4];
  int64_t sref = -1;
} passSt;

struct PassInfo {
  F rt[8];
  uint32_t colorCount = 0, samples = 1, width = 1, height = 1, tailId = 0;
  F dsFormat = F::Undefined;
  bool roDepth = false, roStencil = false;
  uint64_t serial = 0;
} passInfo;
uint64_t passSerial = 0;
std::string passKey;
bool rtKeyValid = false;
std::string rtKey;

void Use(Obj* o) { if (o) o->enc = encEpoch; }
bool Used(const Obj* o) { return o && o->enc == encEpoch; }
wgpu::CommandEncoder& Enc() {
  if (!hasEncoder) { encoder = device.CreateCommandEncoder(); hasEncoder = true; }
  return encoder;
}
void EndOcclusionInPass();
void EndPass() {
  if (!hasPass) return;
  EndOcclusionInPass();
  pass.End();
  pass = nullptr;
  hasPass = false;
  passKey.clear();
  passSt = PassState();
}
void FlushUniforms();
struct Resolves;
Resolves* RecordResolves();
void MapResolves(Resolves*);
struct PendingEvent { uint64_t addr; uint32_t type, seq; uint64_t time; };
std::vector<PendingEvent> pendingEvents;
void QueryStore(uint64_t addr, uint64_t result, uint32_t seq);
bool HavePendingResolves();
uint32_t counterSlot = 0;
void Flush(const char* reason) {
  if (hasEncoder || hasPass) flushReasons[reason]++;
  EndPass();
  FlushUniforms();
  Resolves* res = HavePendingResolves() ? RecordResolves() : nullptr;
  // event queries complete once the GPU has caught up with everything submitted so far
  std::shared_ptr<std::vector<PendingEvent>> evs;
  if (!pendingEvents.empty()) { evs = std::make_shared<std::vector<PendingEvent>>(); evs->swap(pendingEvents); }
  wgpu::CommandBuffer cb;
  if (hasEncoder) {
    cb = encoder.Finish();
    encoder = nullptr;
    hasEncoder = false;
    submits++;
    counterSlot = 0;
  }
  if (cb || evs || res) {
    std::unique_lock<std::mutex> lock(qMu);
    cmdStage = "waiting for room in the submit queue";
    qSpaceCv.wait(lock, [] { return qSubmitsPending < 6; });  // the command thread must not run far ahead of the submits
    cmdStage = "running commands";
    qSubmitsPending++;
    qOps.push_back([cb, evs, res] {
      if (cb) queue.Submit(1, &cb);
      if (evs) queue.OnSubmittedWorkDone(wgpu::CallbackMode::AllowProcessEvents, [evs](wgpu::QueueWorkDoneStatus, wgpu::StringView) {
        for (const PendingEvent& e : *evs) QueryStore(e.addr, e.type == 2 ? e.time : 1, e.seq);
      });
      if (res) MapResolves(res);
      { std::lock_guard<std::mutex> l(qMu); qSubmitsPending--; }
      qSpaceCv.notify_all();
    });
    lock.unlock();
    qCv.notify_one();
  }
  encEpoch++;
}

// ---- pipeline state -------------------------------------------------------------------------------------------------------------

struct VbBind { uint32_t buf = 0, stride = 0, offset = 0; };
struct CbBind { uint32_t buf = 0, first = 0, num = 0; };
struct RtBind { uint32_t tex = 0, mip = 0, slice = 0, fmt = 0; };
struct State {
  uint32_t shader[3] = { 0, 0, 0 }, layout = 0, topo = 4;
  VbBind vb[16];
  struct { uint32_t buf = 0, fmt = 0, offset = 0; } ib;
  CbBind cb[3][16];
  uint32_t srv[3][128] = {}, uav[3][8] = {}, smp[3][16] = {};
  RtBind rt[8];
  struct { uint32_t tex = 0, mip = 0, slice = 0, fmt = 0, flags = 0; } ds;
  struct { uint32_t id = 0, mask = 0xffffffff; float factor[4] = { 1, 1, 1, 1 }; } blend;
  struct { uint32_t id = 0, ref = 0; } dss;
  uint32_t rs = 0;
  uint32_t nViewports = 0, nScissors = 0;
  float viewport[6] = {};
  uint32_t scissor[4] = {};
} S;
bool pdirty = true;
uint32_t bindEpoch[3] = { 0, 0, 0 }, gEpoch = 0;

void ResetState() {
  S = State();
  pdirty = true;
  rtKeyValid = false;
  bindEpoch[0]++; bindEpoch[1]++; bindEpoch[2]++; gEpoch++;
  EndPass();
}

// ---- textures -------------------------------------------------------------------------------------------------------------------

uint64_t texBytesAlive = 0, bufBytesAlive = 0;
uint32_t texCount = 0;

uint64_t TextureBytes(const TexObj& t) {
  uint32_t block = BlockBytes(t.format), texel = TexelBytes(t.format);
  if (!texel) texel = 4;
  uint64_t total = 0;
  for (uint32_t m = 0; m < t.mips; m++) {
    uint64_t mw = std::max(1u, t.width >> m), mh = std::max(1u, t.height >> m), md = t.dim == 3 ? std::max(1u, t.layers >> m) : t.layers;
    total += (block ? ((mw + 3) / 4) * ((mh + 3) / 4) * block : mw * mh * texel) * md;
  }
  return total * t.samples;
}

void CreateTexture(uint32_t id, uint32_t dim, uint32_t w, uint32_t h, uint32_t d, uint32_t mips, uint32_t arraySize, uint32_t dxgi, uint32_t bind, uint32_t samples) {
  Fmt m = MapFormat(dxgi, bind);
  wgpu::TextureUsage usage = wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst;
  if (bind & BIND_SRV) usage |= wgpu::TextureUsage::TextureBinding;
  if (bind & (BIND_RT | BIND_DS)) usage |= wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
  if (bind & BIND_UAV) usage |= wgpu::TextureUsage::StorageBinding;
  if (!(bind & (BIND_SRV | BIND_RT | BIND_DS | BIND_UAV))) usage |= wgpu::TextureUsage::TextureBinding;
  if (samples > 1) usage = (usage & ~(wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::StorageBinding)) | wgpu::TextureUsage::RenderAttachment;
  auto* t = new TexObj();
  t->width = std::max(1u, w); t->height = dim == 1 ? 1 : std::max(1u, h); t->layers = std::max(1u, dim == 3 ? d : arraySize);
  t->mips = std::max(1u, mips); t->samples = std::max(1u, samples); t->dim = dim == 3 ? 3 : dim == 1 ? 1 : 2;
  t->format = m.format; t->a8 = m.a8;
  t->renderable = !!(usage & wgpu::TextureUsage::RenderAttachment);
  wgpu::TextureDescriptor desc;
  desc.size = { t->width, t->height, t->layers };
  desc.mipLevelCount = t->mips;
  desc.sampleCount = t->samples;
  desc.dimension = t->dim == 3 ? wgpu::TextureDimension::e3D : t->dim == 1 ? wgpu::TextureDimension::e1D : wgpu::TextureDimension::e2D;
  desc.format = m.format;
  desc.usage = usage;
  if (m.view != F::Undefined && (!IsBC(m.view) || hasBC)) { desc.viewFormatCount = 1; desc.viewFormats = &m.view; }
  t->tex = device.CreateTexture(&desc);
  t->bytes = TextureBytes(*t);
  texBytesAlive += t->bytes;
  texCount++;
  Put(id, t);
}

void MarkColorDirty(TexObj* t) { if (t && t->proxyFDepth) t->proxyFDepth->dirty = true; }
void MarkDepthDirty(TexObj* t) {
  if (t->proxyDepth) t->proxyDepth->dirty = true;
  if (t->proxyStencil) t->proxyStencil->dirty = true;
}

struct Target { wgpu::TextureView view; F format; uint32_t depthSlice; };
Target TargetView(TexObj* t, uint32_t dxgi, uint32_t mip, uint32_t slice) {
  F fmt = dxgi ? MapFormat(dxgi, 0).format : t->format;
  if (IsDepth(t->format)) fmt = t->format;
  uint64_t key = (uint64_t)fmt << 40 | (uint64_t)mip << 24 | slice;
  auto it = t->views.find(key);
  if (it == t->views.end()) {
    wgpu::TextureViewDescriptor vd;
    vd.format = fmt;
    vd.baseMipLevel = mip;
    vd.mipLevelCount = 1;
    if (t->dim == 3) vd.dimension = wgpu::TextureViewDimension::e3D;
    else { vd.dimension = wgpu::TextureViewDimension::e2D; vd.baseArrayLayer = slice; vd.arrayLayerCount = 1; }
    it = t->views.emplace(key, t->tex.CreateView(&vd)).first;
  }
  return { it->second, fmt, t->dim == 3 ? slice : wgpu::kDepthSliceUndefined };
}

void UploadTexture(uint32_t id, uint32_t mip, uint32_t slice, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, uint32_t d,
                   uint32_t bytesPerRow, uint32_t rowsPerImage, uint32_t size, const uint8_t* data) {
  TexObj* o = Tex(id);
  if (!o) return;
  if (Used(o)) Flush("texture upload");
  MarkColorDirty(o);
  wgpu::TexelCopyTextureInfo dst;
  dst.texture = o->tex;
  dst.mipLevel = mip;
  dst.origin = { x, y, o->dim == 3 ? z : slice };
  wgpu::TexelCopyBufferLayout layout;
  layout.bytesPerRow = bytesPerRow;
  layout.rowsPerImage = rowsPerImage;
  wgpu::Extent3D ext = { w, h, d };
  if (o->a8) {  // one byte per texel -> (0,0,0,a)
    std::vector<uint8_t> out((size_t)w * h * 4, 0);
    for (uint32_t r = 0; r < h; r++) for (uint32_t c = 0; c < w; c++) out[((size_t)r * w + c) * 4 + 3] = data[(size_t)r * bytesPerRow + c];
    layout.bytesPerRow = w * 4;
    QWriteTexture(dst, std::move(out), layout, ext);
    return;
  }
  QWriteTexture(dst, std::vector<uint8_t>(data, data + size), layout, ext);
}

// ---- buffers ----------------------------------------------------------------------------------------------------------------------

void CreateBuffer(uint32_t id, uint32_t size, uint32_t bind, uint32_t misc, uint32_t stride) {
  auto* o = new BufObj();
  o->id = id; o->size = size; o->bind = bind; o->misc = misc; o->stride = stride;
  if (bind & BIND_CB) o->shadow.assign(size, 0);
  wgpu::BufferUsage usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc;
  if (bind & BIND_VB) usage |= wgpu::BufferUsage::Vertex;
  if (bind & BIND_IB) usage |= wgpu::BufferUsage::Index;
  if (bind & (BIND_SRV | BIND_UAV)) usage |= wgpu::BufferUsage::Storage;
  if (misc & 0x10) usage |= wgpu::BufferUsage::Indirect;  // D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS
  if ((bind & (BIND_VB | BIND_IB | BIND_SRV | BIND_UAV)) || (misc & 0x10)) {
    o->bytes = std::max<uint64_t>(4, ((uint64_t)size + 3) & ~3ull);
    wgpu::BufferDescriptor bd;
    bd.size = o->bytes;
    bd.usage = usage;
    o->gpu = device.CreateBuffer(&bd);
    bufBytesAlive += o->bytes;
  }
  Put(id, o);
}

void UploadBuffer(uint32_t id, uint32_t offset, uint32_t size, const uint8_t* data, bool noOverwrite) {
  BufObj* o = Buf(id);
  if (!o) return;
  if (!o->shadow.empty() && (uint64_t)offset + size <= o->shadow.size()) { memcpy(o->shadow.data() + offset, data, size); o->version++; }
  if (!o->gpu) return;
  if ((uint64_t)offset + size > o->bytes) { LogOnce("upbig" + std::to_string(id), "buffer upload past the end of buffer %u", id); return; }
  if (!noOverwrite && Used(o)) Flush("buffer upload");  // a recorded draw still reads the old contents
  uint32_t n = size & ~3u;
  if (n) QWriteBuffer(o->gpu, offset, data, n);
  if (size & 3) {
    uint8_t tail[4] = { 0, 0, 0, 0 };
    memcpy(tail, data + n, size & 3);
    QWriteBuffer(o->gpu, offset + n, tail, 4);
  }
}

// ---- uniform ring: constant buffers are copied per draw (D3D11 semantics) and bound with dynamic offsets -------------------------

const uint32_t RING_CHUNK = 8u << 20, UBO_WINDOW = 65536;
const int FRAME_SLOTS = 3;  // frames in flight: a slot is reused only when the GPU is done with the frame that last used it
struct Chunk {
  wgpu::Buffer buf;
  uint32_t id = 0, base = 0, lo = UINT32_MAX, hi = 0;
  std::vector<uint8_t> stage;  // CPU mirror: one writeBuffer per chunk at the next flush
  bool dirty = false;
};
struct {
  std::vector<std::unique_ptr<Chunk>> slots[FRAME_SLOTS];
  std::vector<std::pair<wgpu::Buffer, uint32_t>> shared;  // chunk k of every frame slot is a region of one buffer
  int slot = 0;
  uint32_t chunk = 0, off = 0, nextId = 1;
  uint64_t frame = 0;
} ubo;
std::vector<Chunk*> dirtyChunks;

Chunk* allocChunk = nullptr;
uint32_t UniformAlloc(uint32_t size) {
  size = (size + 255) & ~255u;
  auto& list = ubo.slots[ubo.slot];
  Chunk* c = ubo.chunk < list.size() ? list[ubo.chunk].get() : nullptr;
  if (!c || ubo.off + size + UBO_WINDOW > RING_CHUNK) {
    if (c) { ubo.chunk++; ubo.off = 0; c = ubo.chunk < list.size() ? list[ubo.chunk].get() : nullptr; }
    if (!c) {
      if (ubo.chunk >= ubo.shared.size()) {
        wgpu::BufferDescriptor bd;
        bd.size = (uint64_t)RING_CHUNK * FRAME_SLOTS;
        bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        ubo.shared.emplace_back(device.CreateBuffer(&bd), ubo.nextId++);
      }
      list.resize(ubo.chunk + 1);
      list[ubo.chunk].reset(c = new Chunk());
      c->buf = ubo.shared[ubo.chunk].first;
      c->id = ubo.shared[ubo.chunk].second;
      c->base = (uint32_t)ubo.slot * RING_CHUNK;
    }
  }
  uint32_t off = ubo.off;
  ubo.off += size;
  allocChunk = c;
  return off;
}
uint8_t* StageReserve(Chunk* c, uint32_t off, uint32_t n) {
  uint32_t end = off + n;
  if (c->stage.size() < end) c->stage.resize(std::min<size_t>(RING_CHUNK, std::max<size_t>(end, std::max<size_t>(c->stage.size() * 2, 262144))));
  c->lo = std::min(c->lo, off);
  c->hi = std::max(c->hi, end);
  if (!c->dirty) { c->dirty = true; dirtyChunks.push_back(c); }
  return c->stage.data();
}
void FlushUniforms() {
  for (Chunk* c : dirtyChunks) {
    uint32_t hi = std::min<uint32_t>((uint32_t)c->stage.size(), (c->hi + 3) & ~3u);
    QWriteBuffer(c->buf, c->base + c->lo, c->stage.data() + c->lo, hi - c->lo);
    c->lo = UINT32_MAX; c->hi = 0; c->dirty = false;
  }
  dirtyChunks.clear();
}

struct Alloc { Chunk* chunk; uint32_t off; };
// Copies the constant buffers a program's merged uniform struct reads into the ring. False: the pack cannot be bound.
bool PackCopy(Program& prog, int stage, Alloc& out) {
  const wgsl::Pack& pack = prog.pack;
  if (pack.members.empty()) return false;
  if (pack.size > UBO_WINDOW) { LogOnce("packbig" + prog.label, "%s: constant buffers total %u bytes (> 64 KB uniform binding limit)", prog.label.c_str(), pack.size); return false; }
  const size_t n = pack.members.size();
  uint32_t ids[16], vers[16], firsts[16];
  BufObj* bufs[16];
  for (size_t i = 0; i < n && i < 16; i++) {
    const CbBind& c = S.cb[stage][pack.members[i].binding & 15];
    bufs[i] = Buf(c.buf);
    ids[i] = bufs[i] ? c.buf : 0; vers[i] = bufs[i] ? bufs[i]->version : 0; firsts[i] = c.first;
  }
  // the same constants already packed this frame by this program and stage: reuse that allocation
  auto& list = prog.packEnts[stage];
  for (PackEnt& en : list) {
    if (en.frame != ubo.frame || en.ids.size() != n) continue;
    if (!memcmp(en.ids.data(), ids, n * 4) && !memcmp(en.vers.data(), vers, n * 4) && !memcmp(en.firsts.data(), firsts, n * 4)) { out = { en.chunk, en.off }; return true; }
  }
  uint32_t aOff = UniformAlloc(pack.size);
  Chunk* aChunk = allocChunk;
  uint8_t* dst = StageReserve(aChunk, aOff, pack.size);
  for (size_t i = 0; i < n; i++) {
    const wgsl::PackMember& m = pack.members[i];
    uint32_t at = aOff + m.offset, len = 0;
    BufObj* o = bufs[i];
    if (o && !o->shadow.empty()) {
      uint64_t from = (uint64_t)firsts[i] * 16;
      if (from < o->size) { len = (uint32_t)std::min<uint64_t>(m.size, o->size - from); memcpy(dst + at, o->shadow.data() + from, len); }
    }
    if (len < m.size) memset(dst + at + len, 0, m.size - len);  // D3D reads zeros where no buffer is bound
  }
  PackEnt en;
  if (list.size() >= 6) { en = std::move(list.front()); list.erase(list.begin()); }
  en.frame = ubo.frame;
  en.ids.assign(ids, ids + n); en.vers.assign(vers, vers + n); en.firsts.assign(firsts, firsts + n);
  en.chunk = aChunk; en.off = aOff;
  list.push_back(std::move(en));
  out = { aChunk, aOff };
  return true;
}

// ---- samplers, shaders, layouts, states -----------------------------------------------------------------------------------------------

wgpu::Sampler MakeSampler(const std::vector<uint32_t>& w) {
  // D3D11_SAMPLER_DESC: Filter, AddressU, AddressV, AddressW, MipLODBias, MaxAnisotropy, ComparisonFunc, BorderColor[4], MinLOD, MaxLOD
  uint32_t filter = w[0];
  bool aniso = (filter & 0x7f) == 0x55;
  wgpu::SamplerDescriptor d;
  d.addressModeU = Address(w[1]); d.addressModeV = Address(w[2]); d.addressModeW = Address(w[3]);
  d.magFilter = (filter >> 2) & 1 ? wgpu::FilterMode::Linear : wgpu::FilterMode::Nearest;
  d.minFilter = (filter >> 4) & 1 ? wgpu::FilterMode::Linear : wgpu::FilterMode::Nearest;
  d.mipmapFilter = filter & 1 ? wgpu::MipmapFilterMode::Linear : wgpu::MipmapFilterMode::Nearest;
  d.lodMinClamp = std::max(0.f, WordFloat(w[11]));
  d.lodMaxClamp = std::min(32.f, std::max(0.f, WordFloat(w[12])));
  if (filter & 0x80) d.compare = Cmp(w[6], wgpu::CompareFunction::Always);
  if (aniso) {
    d.magFilter = d.minFilter = wgpu::FilterMode::Linear;
    d.mipmapFilter = wgpu::MipmapFilterMode::Linear;
    d.maxAnisotropy = (uint16_t)std::max(1u, std::min(16u, w[5]));
  }
  if (d.lodMaxClamp < d.lodMinClamp) d.lodMaxClamp = d.lodMinClamp;
  return device.CreateSampler(&d);
}

std::unordered_map<std::string, uint32_t> internedHashes;
uint32_t nextUid = 1, shaderModuleCount = 0;
double moduleMs = 0;

bool ReadFile(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}
wgpu::ShaderModule MakeModule(const std::string& text, const std::string& label) {
  wgpu::ShaderSourceWGSL src;
  src.code = wgpu::StringView(text.data(), text.size());
  wgpu::ShaderModuleDescriptor smd;
  smd.nextInChain = &src;
  smd.label = wgpu::StringView(label.data(), label.size());
  double t0 = host_now_ms();
  wgpu::ShaderModule m = device.CreateShaderModule(&smd);
  moduleMs += host_now_ms() - t0;
  shaderModuleCount++;
  return m;
}

// The translated WGSL of a shader, loaded and compiled on first use. One program per shader hash: the engine creates several
// shader objects for the same shader, and pipelines compiled ahead (below) need programs before any object exists.
std::unordered_map<std::string, std::unique_ptr<Program>> programsByHash;
Program* ProgramFor(const std::string& hash, uint32_t stage) {
  auto it = programsByHash.find(hash);
  if (it != programsByHash.end()) return it->second.get();
  std::unique_ptr<Program>& slot = programsByHash[hash];  // stays null when the shader is unusable
  std::string source, dir = host_shader_dir();
  if (!ReadFile(dir + "/" + hash + ".wgsl", source)) {
    LogOnce("noshader" + hash, "no WGSL for %s shader %s", stage == 0 ? "vs" : stage == 1 ? "ps" : "cs", hash.c_str());
    return nullptr;
  }
  auto owned = std::make_unique<Program>();
  Program& p = *owned;
  p.label = hash;
  p.text = wgsl::PackUniforms(source, p.pack);
  p.module = MakeModule(p.text, p.label);
  p.bindings = wgsl::ParseBindings(p.text);
  p.outputs = wgsl::FragmentOutputs(p.text);
  p.flat = wgsl::FlatLocations(p.text);
  std::string consts;
  if (ReadFile(dir + "/" + hash + ".consts.json", consts)) {
    std::vector<uint32_t> data;
    for (const char* c = consts.c_str(); *c;) {
      if (*c == '-' || (*c >= '0' && *c <= '9')) { char* e; double v = strtod(c, &e); data.push_back((uint32_t)(int64_t)v); data.push_back(0); data.push_back(0); data.push_back(0); c = e; }
      else c++;
    }
    if (data.empty()) data.assign(4, 0);
    wgpu::BufferDescriptor bd;
    bd.size = std::max<size_t>(16, data.size() * 4);
    bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    p.consts = device.CreateBuffer(&bd);
    QWriteBuffer(p.consts, 0, data.data(), data.size() * 4);
  }
  for (const Binding& b : p.bindings) if (b.group == 0 || b.group == 1) { p.groupBindings[b.group].push_back(b); p.hasGroup[b.group] = true; }
  for (auto& g : p.groupBindings) std::sort(g.begin(), g.end(), [](const Binding& a, const Binding& b) { return a.binding < b.binding; });
  // the shader reads vertex_index / instance_index: the draw's starts are folded into the buffer offsets for those
  p.vid = wgsl::HasWord(p.text, "vertex_index");
  p.iid = wgsl::HasWord(p.text, "instance_index");
  p.uid = nextUid++;
  slot = std::move(owned);
  return slot.get();
}
Program* ShaderProgram(ShaderObj* sh) {
  if (!sh->tried) { sh->tried = true; sh->prog = ProgramFor(sh->hash, sh->stage); }
  return sh->prog;
}

void CreateShader(uint64_t p) {
  auto* o = new ShaderObj();
  uint32_t id = *U32(p);
  o->stage = *U32(p + 1);
  char hash[17];
  o->hashLo = *U32(p + 2); o->hashHi = *U32(p + 3);
  snprintf(hash, sizeof hash, "%08x%08x", o->hashHi, o->hashLo);
  o->hash = hash;
  uint32_t nin = *U32(p + 4);
  for (uint32_t i = 0; i < nin; i++) o->sig.push_back({ *U32(p + 5 + i * 4), *U32(p + 6 + i * 4), *U32(p + 7 + i * 4), *U32(p + 8 + i * 4) });
  auto it = internedHashes.find(o->hash);
  if (it == internedHashes.end()) it = internedHashes.emplace(o->hash, (uint32_t)internedHashes.size() + 1).first;
  o->hid = it->second;
  Put(id, o);
}

void CreateLayout(uint64_t p) {
  // id, n, then n x [semHash, semIndex, dxgi, slot, offset, slotClass, stepRate]
  auto* o = new LayoutObj();
  uint32_t n = *U32(p + 1), seen = 0;
  for (uint32_t i = 0; i < n; i++) {
    uint64_t q = p + 2 + i * 7;
    LayoutEl e = { *U32(q), *U32(q + 1), *U32(q + 2), *U32(q + 3), *U32(q + 4), *U32(q + 5), *U32(q + 6) };
    o->els.push_back(e);
    if (e.slot < 16 && !((seen >> e.slot) & 1)) { seen |= 1u << e.slot; o->slots.push_back({ e.slot, e.cls }); }
  }
  Put(*U32(p), o);
}

void CreateState(uint64_t p, uint32_t n) {
  auto* o = new StateObj();
  o->skind = *U32(p + 1);
  o->words.assign(U32(p + 2), U32(p + n));
  if (o->skind == 3 && o->words.size() >= 13) o->sampler = MakeSampler(o->words);
  Put(*U32(p), o);
}

void CreateView(uint64_t p, OK kind) {
  // id, resKind (0 texture, 1 buffer), resId, dxgi, viewDim, a, b, c, d (UAV: flags)
  auto* o = new ViewObj(kind);
  o->isBuf = *U32(p + 1) == 1; o->res = *U32(p + 2); o->fmt = *U32(p + 3); o->vdim = *U32(p + 4);
  o->a = *U32(p + 5); o->b = *U32(p + 6); o->c = *U32(p + 7); o->d = *U32(p + 8);
  Put(*U32(p), o);
}

// ---- dummies for unbound slots --------------------------------------------------------------------------------------------------------

std::unordered_map<uint32_t, wgpu::TextureView> dummyTex;
std::unordered_map<uint32_t, wgpu::Buffer> dummyBufs;
wgpu::Sampler dummySmp, dummyCmp;

wgpu::TextureView DummyTexture(const Binding& b) {
  uint32_t key = (uint32_t)b.dim << 16 | (uint32_t)b.sample << 4 | (b.multisampled ? 1 : 0);
  auto it = dummyTex.find(key);
  if (it != dummyTex.end()) return it->second;
  wgpu::TextureDescriptor d;
  d.format = b.sample == wgpu::TextureSampleType::Uint ? F::RGBA8Uint : b.sample == wgpu::TextureSampleType::Sint ? F::RGBA8Sint
           : b.sample == wgpu::TextureSampleType::Depth ? F::Depth32Float : F::RGBA8Unorm;
  bool cube = b.dim == wgpu::TextureViewDimension::Cube || b.dim == wgpu::TextureViewDimension::CubeArray;
  d.size = { 1, 1, cube ? 6u : 1u };
  d.dimension = b.dim == wgpu::TextureViewDimension::e3D ? wgpu::TextureDimension::e3D : b.dim == wgpu::TextureViewDimension::e1D ? wgpu::TextureDimension::e1D : wgpu::TextureDimension::e2D;
  d.usage = wgpu::TextureUsage::TextureBinding | (b.multisampled ? wgpu::TextureUsage::RenderAttachment : wgpu::TextureUsage::CopyDst);
  d.sampleCount = b.multisampled ? 4 : 1;
  wgpu::TextureViewDescriptor vd;
  vd.dimension = b.dim;
  wgpu::TextureView v = device.CreateTexture(&d).CreateView(&vd);
  dummyTex.emplace(key, v);
  return v;
}
wgpu::Sampler DummySampler(bool cmp) {
  wgpu::Sampler& s = cmp ? dummyCmp : dummySmp;
  if (!s) {
    wgpu::SamplerDescriptor d;
    if (cmp) d.compare = wgpu::CompareFunction::Always;
    s = device.CreateSampler(&d);
  }
  return s;
}
// one per binding: writable storage buffers bound twice in a dispatch are an aliasing error
wgpu::Buffer DummyBuffer(uint32_t binding) {
  wgpu::Buffer& b = dummyBufs[binding];
  if (!b) {
    wgpu::BufferDescriptor d;
    d.size = 65536;
    d.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
    b = device.CreateBuffer(&d);
  }
  return b;
}

// ---- depth textures sampled as float -----------------------------------------------------------------------------------------------------
// D3D11 lets a shader read a depth texture as a float texture; WebGPU only binds it as `depth` and the translated code uses
// texture_2d<f32>. A proxy r32float (depth) or r8uint (stencil) texture is refreshed by a pass whenever the depth texture was
// drawn into since the last read. The reverse (SampleCmp on a float texture) keeps a depth32float copy.

const char* kFullscreenVs = "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f { var p = array<vec2f, 3>(vec2f(-1, -1), vec2f(3, -1), vec2f(-1, 3)); return vec4f(p[i], 0, 1); }\n";
wgpu::RenderPipeline proxyPipelines[2], f2dPipeline;

wgpu::RenderPipeline SimplePipeline(const std::string& code, F target, bool depthOut) {
  wgpu::ShaderModule module = MakeModule(code, "internal");
  wgpu::RenderPipelineDescriptor d;
  d.vertex.module = module;
  d.vertex.entryPoint = "vs";
  wgpu::ColorTargetState ct;
  ct.format = target;
  wgpu::FragmentState fs;
  fs.module = module;
  fs.entryPoint = "fs";
  fs.targetCount = depthOut ? 0 : 1;
  fs.targets = depthOut ? nullptr : &ct;
  d.fragment = &fs;
  wgpu::DepthStencilState dss;
  if (depthOut) {
    dss.format = F::Depth32Float;
    dss.depthWriteEnabled = wgpu::OptionalBool::True;
    dss.depthCompare = wgpu::CompareFunction::Always;
    d.depthStencil = &dss;
  }
  return device.CreateRenderPipeline(&d);
}

wgpu::BindGroup OneTextureGroup(const wgpu::RenderPipeline& pl, const wgpu::TextureView& view, const wgpu::Sampler* sampler = nullptr) {
  wgpu::BindGroupEntry e[2];
  e[0].binding = 0;
  e[0].textureView = view;
  if (sampler) { e[1].binding = 1; e[1].sampler = *sampler; }
  wgpu::BindGroupDescriptor bd;
  bd.layout = pl.GetBindGroupLayout(0);
  bd.entryCount = sampler ? 2 : 1;
  bd.entries = e;
  return device.CreateBindGroup(&bd);
}
wgpu::TextureView LevelView(const wgpu::Texture& tex, uint32_t mip, uint32_t layer, wgpu::TextureAspect aspect = wgpu::TextureAspect::All) {
  wgpu::TextureViewDescriptor vd;
  vd.dimension = wgpu::TextureViewDimension::e2D;
  vd.baseMipLevel = mip; vd.mipLevelCount = 1; vd.baseArrayLayer = layer; vd.arrayLayerCount = 1;
  vd.aspect = aspect;
  return tex.CreateView(&vd);
}
std::unique_ptr<TexObj> MakeProxy(const TexObj& t, F format) {
  auto px = std::make_unique<TexObj>();
  px->width = t.width; px->height = t.height; px->layers = t.layers; px->mips = t.mips; px->format = format;
  wgpu::TextureDescriptor d;
  d.size = { t.width, t.height, t.layers };
  d.mipLevelCount = t.mips;
  d.format = format;
  d.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::RenderAttachment;
  px->tex = device.CreateTexture(&d);
  px->dirty = true;
  return px;
}

// The proxy of depth texture `t` (stencil: its stencil plane), refreshed if `t` changed since the last refresh.
TexObj* DepthProxy(TexObj* t, bool stencil) {
  std::unique_ptr<TexObj>& slot = stencil ? t->proxyStencil : t->proxyDepth;
  if (!slot) slot = MakeProxy(*t, stencil ? F::R8Uint : F::R32Float);
  TexObj* px = slot.get();
  if (!px->dirty) return px;
  EndPass();
  wgpu::RenderPipeline& pl = proxyPipelines[stencil ? 1 : 0];
  if (!pl) pl = stencil
    ? SimplePipeline(std::string("@group(0) @binding(0) var src: texture_2d<u32>;\n") + kFullscreenVs + "@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4<u32> { return textureLoad(src, vec2i(pos.xy), 0); }", F::R8Uint, false)
    : SimplePipeline(std::string("@group(0) @binding(0) var src: texture_depth_2d;\n") + kFullscreenVs + "@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f { return vec4f(textureLoad(src, vec2i(pos.xy), 0), 0, 0, 1); }", F::R32Float, false);
  if (px->passes.empty())
    for (uint32_t mip = 0; mip < t->mips; mip++) for (uint32_t layer = 0; layer < t->layers; layer++)
      px->passes.push_back({ LevelView(px->tex, mip, layer), OneTextureGroup(pl, LevelView(t->tex, mip, layer, stencil ? wgpu::TextureAspect::StencilOnly : wgpu::TextureAspect::DepthOnly)) });
  for (const ProxyPass& r : px->passes) {
    wgpu::RenderPassColorAttachment ca;
    ca.view = r.dst; ca.loadOp = wgpu::LoadOp::Clear; ca.storeOp = wgpu::StoreOp::Store;
    wgpu::RenderPassDescriptor rp;
    rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
    wgpu::RenderPassEncoder p = Enc().BeginRenderPass(&rp);
    p.SetPipeline(pl); p.SetBindGroup(0, r.bg); p.Draw(3); p.End();
    statProxyPasses++;
  }
  Use(t);
  px->dirty = false;
  return px;
}

TexObj* FloatToDepthProxy(TexObj* t) {
  if (!t->proxyFDepth) t->proxyFDepth = MakeProxy(*t, F::Depth32Float);
  TexObj* px = t->proxyFDepth.get();
  if (!px->dirty) return px;
  EndPass();
  if (!f2dPipeline) f2dPipeline = SimplePipeline(std::string("@group(0) @binding(0) var src: texture_2d<f32>;\n") + kFullscreenVs + "@fragment fn fs(@builtin(position) pos: vec4f) -> @builtin(frag_depth) f32 { return textureLoad(src, vec2i(pos.xy), 0).x; }", F::Undefined, true);
  if (px->passes.empty())
    for (uint32_t mip = 0; mip < t->mips; mip++) for (uint32_t layer = 0; layer < t->layers; layer++)
      px->passes.push_back({ LevelView(px->tex, mip, layer), OneTextureGroup(f2dPipeline, LevelView(t->tex, mip, layer)) });
  for (const ProxyPass& r : px->passes) {
    wgpu::RenderPassDepthStencilAttachment da;
    da.view = r.dst; da.depthLoadOp = wgpu::LoadOp::Clear; da.depthStoreOp = wgpu::StoreOp::Store; da.depthClearValue = 1;
    wgpu::RenderPassDescriptor rp;
    rp.depthStencilAttachment = &da;
    wgpu::RenderPassEncoder p = Enc().BeginRenderPass(&rp);
    p.SetPipeline(f2dPipeline); p.SetBindGroup(0, r.bg); p.Draw(3); p.End();
    statProxyPasses++;
  }
  Use(t);
  px->dirty = false;
  return px;
}

bool sawProxy = false;
// The texture view of an SRV in the dimension the shader declares; null: bind a dummy.
wgpu::TextureView SrvView(ViewObj* srv, const Binding& b) {
  TexObj* t = Tex(srv->res);
  if (!t) return nullptr;
  bool proxied = false;
  if (IsDepth(t->format) && b.sample != wgpu::TextureSampleType::Depth) { t = DepthProxy(t, b.sample == wgpu::TextureSampleType::Uint); proxied = true; sawProxy = true; }
  else if (!IsDepth(t->format) && b.sample == wgpu::TextureSampleType::Depth) { t = FloatToDepthProxy(t); proxied = true; sawProxy = true; }
  uint32_t key = (uint32_t)b.dim << 1 | (proxied ? 1 : 0);
  auto it = srv->views.find(key);
  if (it != srv->views.end() && it->second.source == t->tex.Get()) return it->second.view;
  uint32_t total = t->mips, layers = t->dim == 3 ? 1 : t->layers;
  uint32_t mipCount = srv->b == 0xffffffff || srv->b == 0 ? total - std::min(srv->a, total - 1) : srv->b;
  bool cube = b.dim == wgpu::TextureViewDimension::Cube || b.dim == wgpu::TextureViewDimension::CubeArray;
  if (cube && (srv->c >= layers || layers - srv->c < 6)) {
    LogOnce("cubeview" + std::to_string(srv->res), "cube SRV over a texture with %u layer(s): bound as an empty cube", layers);
    return nullptr;
  }
  if ((b.dim == wgpu::TextureViewDimension::e3D) != (t->dim == 3) || b.multisampled != (t->samples > 1)) return nullptr;
  wgpu::TextureViewDescriptor vd;
  vd.dimension = b.dim;
  vd.baseMipLevel = std::min(srv->a, total - 1);
  vd.mipLevelCount = std::max(1u, std::min(mipCount, total - vd.baseMipLevel));
  if (b.dim != wgpu::TextureViewDimension::e3D && b.dim != wgpu::TextureViewDimension::e1D) {
    uint32_t first = std::min(srv->c, layers - 1);
    uint32_t cnt = b.dim == wgpu::TextureViewDimension::Cube ? 6 : b.dim == wgpu::TextureViewDimension::e2D ? 1 : (srv->d ? srv->d : layers - first);
    vd.baseArrayLayer = first;
    vd.arrayLayerCount = std::max(1u, std::min(cnt, layers - first));
  }
  F f = t->format;
  if (srv->fmt && !IsDepth(f)) { F mf = MapFormat(srv->fmt, 0).format; if (mf != f && IsSrgb(f) != IsSrgb(mf) && (!IsBC(mf) || hasBC)) vd.format = mf; }
  if (IsDepth(f) && !proxied) vd.aspect = srv->fmt == 47 || srv->fmt == 22 ? wgpu::TextureAspect::StencilOnly : wgpu::TextureAspect::DepthOnly;
  wgpu::TextureView v = t->tex.CreateView(&vd);
  srv->views[key] = { v, t->tex.Get() };
  return v;
}

// ---- pipelines ----------------------------------------------------------------------------------------------------------------------

struct VbReq { uint32_t slot; bool instance; uint32_t stride, last; };
struct Pipeline {
  wgpu::RenderPipeline pl;
  std::vector<VbReq> vbReq;  // what each vertex buffer slot must hold for n elements: (n - 1) * stride + last
  // Compiled off the command thread: a synchronous compile stalls every frame that first shows a new material (tens to hundreds of
  // ms each, dozens while a new area streams in). Draws that need a pipeline still compiling are skipped: the object appears a
  // frame or two later. `pl` is written by the completion callback before `state` leaves Pending.
  enum { Pending, Ready, Failed };
  std::atomic<int> state{ Pending };
};
std::atomic<int> pipelinesPending{ 0 };
uint64_t pendingSkips = 0;
std::unordered_map<std::vector<uint32_t>, Pipeline*, KeyHash> pipelinesByIds, pipelinesByContent;
std::unordered_map<std::vector<uint32_t>, uint32_t, KeyHash> internedTails;
wgpu::BindGroupLayout emptyBgl;

wgpu::BindGroupLayoutEntry LayoutEntry(const Binding& b, int stage) {
  wgpu::BindGroupLayoutEntry e;
  e.binding = b.binding;
  e.visibility = stage == 0 ? wgpu::ShaderStage::Vertex : stage == 1 ? wgpu::ShaderStage::Fragment : wgpu::ShaderStage::Compute;
  switch (b.kind) {
    case Kind::Uniform: e.buffer.type = wgpu::BufferBindingType::Uniform; e.buffer.hasDynamicOffset = b.binding != 15; break;  // binding 0 = the packed constant buffers
    case Kind::Storage: e.buffer.type = wgpu::BufferBindingType::Storage; break;
    case Kind::ReadOnlyStorage: e.buffer.type = wgpu::BufferBindingType::ReadOnlyStorage; break;
    case Kind::Sampler: e.sampler.type = wgpu::SamplerBindingType::Filtering; break;
    case Kind::SamplerComparison: e.sampler.type = wgpu::SamplerBindingType::Comparison; break;
    case Kind::Texture: e.texture.sampleType = b.sample; e.texture.viewDimension = b.dim; e.texture.multisampled = b.multisampled; break;
    case Kind::StorageTexture: e.storageTexture.access = b.access; e.storageTexture.format = b.format; e.storageTexture.viewDimension = b.dim; break;
  }
  return e;
}
BglRef MakeBgl(const std::vector<Binding>& bindings, int stage) {
  std::vector<wgpu::BindGroupLayoutEntry> entries;
  for (const Binding& b : bindings) entries.push_back(LayoutEntry(b, stage));
  wgpu::BindGroupLayoutDescriptor d;
  d.entryCount = entries.size();
  d.entries = entries.data();
  return { device.CreateBindGroupLayout(&d), nextUid++ };
}
// A draw program's bindings all sit in one group: 0 for a vertex shader, 1 for a pixel shader.
BglRef& BglFor(Program& prog, int stage) {
  if (!prog.bgl[stage].layout) prog.bgl[stage] = MakeBgl(prog.groupBindings[stage], stage);
  return prog.bgl[stage];
}

// WebGPU requires the interpolation of a varying to match between the stages; HLSL only declares `nointerpolation` on the
// pixel shader input. The vertex shader gets a variant with those outputs flat.
wgpu::ShaderModule VertexModule(Program& vs, const std::vector<int>& flat) {
  if (flat.empty()) return vs.module;
  std::string key;
  for (int l : flat) key += std::to_string(l) + ",";
  auto it = vs.variants.find(key);
  if (it != vs.variants.end()) return it->second;
  wgpu::ShaderModule m = MakeModule(wgsl::MarkFlat(vs.text, flat), vs.label + " [flat " + key + "]");
  vs.variants.emplace(key, m);
  return m;
}
// D3D11 alpha-to-coverage on a single-sample target turns the alpha of target 0 into a coverage of 0 or 1: the foliage shaders
// rely on it instead of clip(). WebGPU only honours the flag with MSAA, so the module gets an explicit alpha test.
wgpu::ShaderModule PixelModule(Program& ps, bool a2c) {
  if (!a2c) return ps.module;
  if (!ps.a2cTried) {
    ps.a2cTried = true;
    std::string text;
    if (wgsl::AddAlphaTest(ps.text, text)) ps.a2c = MakeModule(text, ps.label + " [alpha test]");
  }
  return ps.a2c ? ps.a2c : ps.module;
}

// A pipeline by content: everything it is built from as plain numbers, with no object ids. The same pipeline is found again
// for other state objects, and it can be rebuilt in a later run before anything draws with it (see "compiled ahead" below).
// The encoded form (a vector of words) is the cache key and the record in the pipeline file.
struct Recipe {
  uint32_t vsLo = 0, vsHi = 0, psLo = 0, psHi = 0;
  bool hasPs = false;
  std::vector<SigEl> sig;      // the vertex shader's input signature
  std::vector<LayoutEl> els;
  uint32_t strides[16] = {};
  uint32_t topo = 4, stripFmt = 0;
  uint32_t colorCount = 0, samples = 1;
  F rt[8], dsFormat = F::Undefined;
  bool roDepth = false, roStencil = false, hasBlend = false, hasDss = false, hasRs = false;
  std::vector<uint32_t> blend, dss, rs;
};
const uint32_t kRecipeVersion = 1;

void EncodeState(std::vector<uint32_t>& k, const StateObj* st) {
  k.push_back(st ? (uint32_t)st->words.size() : 0xffffffffu);
  if (st) k.insert(k.end(), st->words.begin(), st->words.end());
}
bool DecodeRecipe(const std::vector<uint32_t>& k, Recipe& r) {
  size_t i = 0;
  auto take = [&](uint32_t& v) { if (i >= k.size()) return false; v = k[i++]; return true; };
  uint32_t version, hasPs, n;
  if (!take(version) || version != kRecipeVersion) return false;
  if (!take(r.vsLo) || !take(r.vsHi) || !take(r.psLo) || !take(r.psHi) || !take(hasPs)) return false;
  r.hasPs = hasPs != 0;
  if (!take(n) || n > 64 || i + n * 4 > k.size()) return false;
  for (uint32_t e = 0; e < n; e++, i += 4) r.sig.push_back({ k[i], k[i + 1], k[i + 2], k[i + 3] });
  if (!take(n) || n > 64 || i + n * 7 > k.size()) return false;
  for (uint32_t e = 0; e < n; e++, i += 7) r.els.push_back({ k[i], k[i + 1], k[i + 2], k[i + 3], k[i + 4], k[i + 5], k[i + 6] });
  if (i + 16 + 2 + 1 + 8 + 3 > k.size()) return false;
  for (int e = 0; e < 16; e++) r.strides[e] = k[i++];
  r.topo = k[i++]; r.stripFmt = k[i++];
  r.colorCount = k[i++];
  if (r.colorCount > 8) return false;
  for (int e = 0; e < 8; e++) r.rt[e] = (F)k[i++];
  r.dsFormat = (F)k[i++]; r.samples = k[i++];
  uint32_t ro = k[i++];
  r.roDepth = ro & 1; r.roStencil = (ro & 2) != 0;
  struct { bool* has; std::vector<uint32_t>* words; } states[3] = { { &r.hasBlend, &r.blend }, { &r.hasDss, &r.dss }, { &r.hasRs, &r.rs } };
  for (auto& st : states) {
    if (!take(n)) return false;
    *st.has = n != 0xffffffffu;
    if (!*st.has) continue;
    if (n > 256 || i + n > k.size()) return false;
    st.words->assign(k.begin() + i, k.begin() + i + n);
    i += n;
  }
  return i == k.size();
}

Pipeline* BuildPipeline(const Recipe& r) {
  auto* out = new Pipeline();
  char hash[17];
  snprintf(hash, sizeof hash, "%08x%08x", r.vsHi, r.vsLo);
  Program* vsp = ProgramFor(hash, 0);
  Program* ps = nullptr;
  if (r.hasPs) { snprintf(hash, sizeof hash, "%08x%08x", r.psHi, r.psLo); ps = ProgramFor(hash, 1); }
  if (!vsp || (r.hasPs && !ps)) { out->state = Pipeline::Failed; return out; }
  Program& vs = *vsp;
  wgpu::RenderPipelineDescriptor desc;
  std::string label = vs.label + " + " + (ps ? ps->label : "-");
  desc.label = wgpu::StringView(label.data(), label.size());
  wgpu::BindGroupLayout bgls[2] = { vs.hasGroup[0] ? BglFor(vs, 0).layout : emptyBgl, ps && ps->hasGroup[1] ? BglFor(*ps, 1).layout : emptyBgl };
  wgpu::PipelineLayoutDescriptor pld;
  pld.bindGroupLayoutCount = 2;
  pld.bindGroupLayouts = bgls;
  desc.layout = device.CreatePipelineLayout(&pld);

  // D3D input layout elements matched to the vertex shader's input signature by semantic
  std::vector<wgpu::VertexAttribute> attrs[16];
  bool instanced[16] = {}, used[16] = {};
  uint32_t maxSlot = 0;
  for (const LayoutEl& e : r.els) {
    if (e.slot >= 16) continue;
    const SigEl* sg = nullptr;
    for (const SigEl& x : r.sig) if (x.sem == e.sem && x.idx == e.idx) { sg = &x; break; }
    if (!sg) continue;  // element the shader does not read
    VFormat f = VertexFormatOf(e.fmt);
    if (!f.bytes) { LogOnce("vf" + std::to_string(e.fmt), "unsupported vertex format DXGI %u", e.fmt); continue; }
    if (!used[e.slot]) { used[e.slot] = true; instanced[e.slot] = e.cls == 1; maxSlot = std::max(maxSlot, e.slot + 1); }
    wgpu::VertexAttribute a;
    a.shaderLocation = sg->reg; a.offset = e.offset; a.format = f.format;
    attrs[e.slot].push_back(a);
    uint32_t last = e.offset + f.bytes;
    bool found = false;
    for (VbReq& q : out->vbReq) if (q.slot == e.slot) { q.last = std::max(q.last, last); found = true; }
    if (!found) out->vbReq.push_back({ e.slot, e.cls == 1, r.strides[e.slot], last });
  }
  wgpu::VertexBufferLayout vbl[16];
  for (uint32_t i = 0; i < maxSlot; i++) {
    if (!used[i]) { vbl[i].stepMode = wgpu::VertexStepMode::Undefined; continue; }
    vbl[i].arrayStride = r.strides[i];
    vbl[i].stepMode = instanced[i] ? wgpu::VertexStepMode::Instance : wgpu::VertexStepMode::Vertex;
    vbl[i].attributeCount = attrs[i].size();
    vbl[i].attributes = attrs[i].data();
  }
  desc.vertex.module = VertexModule(vs, ps ? ps->flat : std::vector<int>());
  desc.vertex.entryPoint = "main";
  desc.vertex.bufferCount = maxSlot;
  desc.vertex.buffers = vbl;
  desc.primitive.topology = TopologyOf(r.topo).topology;
  desc.primitive.stripIndexFormat = (wgpu::IndexFormat)r.stripFmt;
  if (r.hasRs && r.rs.size() >= 8) {
    // FillMode, CullMode, FrontCCW, DepthBias, DepthBiasClamp, SlopeScaledDepthBias, DepthClip, Scissor, MS, AA line
    desc.primitive.cullMode = r.rs[1] == 2 ? wgpu::CullMode::Front : r.rs[1] == 3 ? wgpu::CullMode::Back : wgpu::CullMode::None;
    desc.primitive.frontFace = r.rs[2] ? wgpu::FrontFace::CCW : wgpu::FrontFace::CW;
  } else { desc.primitive.cullMode = wgpu::CullMode::Back; desc.primitive.frontFace = wgpu::FrontFace::CW; }

  wgpu::DepthStencilState ds;
  if (r.dsFormat != F::Undefined) {
    ds.format = r.dsFormat;
    ds.depthWriteEnabled = wgpu::OptionalBool::False;
    ds.depthCompare = wgpu::CompareFunction::Always;
    bool stencilSet = false;
    if (r.hasDss && r.dss.size() >= 13) {
      const auto& w = r.dss;
      if (w[0]) { ds.depthWriteEnabled = w[1] == 1 ? wgpu::OptionalBool::True : wgpu::OptionalBool::False; ds.depthCompare = Cmp(w[2], wgpu::CompareFunction::Less); }
      if (HasStencil(r.dsFormat) && w[3]) {
        stencilSet = true;
        ds.stencilReadMask = w[4] & 0xff; ds.stencilWriteMask = (w[4] >> 8) & 0xff;
        ds.stencilFront.compare = Cmp(w[8], wgpu::CompareFunction::Always); ds.stencilFront.failOp = StencilOp(w[5]); ds.stencilFront.depthFailOp = StencilOp(w[6]); ds.stencilFront.passOp = StencilOp(w[7]);
        ds.stencilBack.compare = Cmp(w[12], wgpu::CompareFunction::Always); ds.stencilBack.failOp = StencilOp(w[9]); ds.stencilBack.depthFailOp = StencilOp(w[10]); ds.stencilBack.passOp = StencilOp(w[11]);
      }
    }
    if (r.roDepth) ds.depthWriteEnabled = wgpu::OptionalBool::False;
    if (r.roStencil && stencilSet) ds.stencilWriteMask = 0;
    if (r.hasRs && r.rs.size() >= 6) { ds.depthBias = (int32_t)r.rs[3]; ds.depthBiasSlopeScale = WordFloat(r.rs[5]); ds.depthBiasClamp = WordFloat(r.rs[4]); }
    desc.depthStencil = &ds;
  }

  // the targets must match the pass attachments slot by slot; slots the shader does not write get a zero write mask
  wgpu::ColorTargetState targets[8];
  wgpu::BlendState blends[8];
  wgpu::FragmentState fs;
  const bool a2cState = r.hasBlend && !r.blend.empty() && r.blend[0];
  if (ps) {
    for (uint32_t i = 0; i < r.colorCount; i++) {
      F f = r.rt[i];
      targets[i].format = f;
      if (f == F::Undefined) continue;
      if (std::find(ps->outputs.begin(), ps->outputs.end(), (int)i) == ps->outputs.end()) { targets[i].writeMask = wgpu::ColorWriteMask::None; continue; }
      targets[i].writeMask = wgpu::ColorWriteMask::All;
      if (r.hasBlend && r.blend.size() >= 66) {
        // D3D11_BLEND_DESC: AlphaToCoverage, Independent, RT[8] x 8 words {enable, src, dst, op, srcA, dstA, opA, mask}
        const auto& w = r.blend;
        uint32_t q = 2 + (w[1] ? i : 0) * 8;
        targets[i].writeMask = (wgpu::ColorWriteMask)(w[q + 7] & 0xf);
        if (w[q]) {
          blends[i].color = { BlendOp(w[q + 3]), BlendFactor(w[q + 1], wgpu::BlendFactor::One), BlendFactor(w[q + 2], wgpu::BlendFactor::Zero) };
          blends[i].alpha = { BlendOp(w[q + 6]), BlendFactor(w[q + 4], wgpu::BlendFactor::One), BlendFactor(w[q + 5], wgpu::BlendFactor::Zero) };
          targets[i].blend = &blends[i];
        }
      }
    }
    for (int l : ps->outputs)
      if (l < 0 || l >= (int)r.colorCount || r.rt[l] == F::Undefined) LogOnce("psout" + ps->label, "%s writes colour output %d with no render target bound", ps->label.c_str(), l);
    bool a2c = a2cState && r.samples <= 1 && std::find(ps->outputs.begin(), ps->outputs.end(), 0) != ps->outputs.end();
    fs.module = PixelModule(*ps, a2c);
    fs.entryPoint = "main";
    fs.targetCount = r.colorCount;
    fs.targets = targets;
    desc.fragment = &fs;
  }
  if (r.samples > 1) { desc.multisample.count = r.samples; desc.multisample.alphaToCoverageEnabled = a2cState; }

  double t0 = host_now_ms();
  pipelineCount++;
  if (getenv("GTA_SYNC_PIPELINES")) {
    bool failed = false;
    device.PushErrorScope(wgpu::ErrorFilter::Validation);
    out->pl = device.CreateRenderPipeline(&desc);
    instance.WaitAny(device.PopErrorScope(wgpu::CallbackMode::WaitAnyOnly, [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView msg) {
      if (type == wgpu::ErrorType::NoError) return;
      failed = true;
      LogOnce("plfail" + label, "pipeline failed (%s): %.*s", label.c_str(), (int)std::min<size_t>(msg.length, 600), msg.data);
    }), UINT64_MAX);
    out->state = failed ? Pipeline::Failed : Pipeline::Ready;
  } else {
    pipelinesPending++;
    device.CreateRenderPipelineAsync(&desc, wgpu::CallbackMode::AllowProcessEvents, [out, label](wgpu::CreatePipelineAsyncStatus status, wgpu::RenderPipeline p, wgpu::StringView msg) {
      if (status == wgpu::CreatePipelineAsyncStatus::Success) { out->pl = std::move(p); out->state = Pipeline::Ready; }
      else { host_log("[gpu] pipeline failed (%s): %.*s", label.c_str(), (int)std::min<size_t>(msg.length, 600), msg.data); out->state = Pipeline::Failed; }
      pipelinesPending--;
    });
  }
  pipeMs += host_now_ms() - t0;
  return out;
}

// ---- pipelines compiled ahead -------------------------------------------------------------------------------------------------------
// A draw whose pipeline is still compiling is skipped, so the first time a material is seen its object appears late. Every
// recipe ever used is therefore kept in <user folder>/pipelines.bin, and on the next launch they are all compiled while the
// game loads: a draw then finds its pipeline ready.

const size_t RECIPES_MAX = 8000;
std::vector<std::vector<uint32_t>> aheadList;
size_t aheadPos = 0;
uint64_t aheadMade = 0, aheadUsed = 0;
double aheadT0 = 0;
FILE* recipeFile = nullptr;
std::unordered_map<std::vector<uint32_t>, bool, KeyHash> aheadKeys;  // recipe -> compiled ahead and not drawn with yet

void LoadRecipes() {
  std::string path = std::string(host_user_dir()) + "/pipelines.bin";
  if (FILE* f = fopen(path.c_str(), "rb")) {
    uint32_t n;
    while (fread(&n, 4, 1, f) == 1 && n && n < 4096) {
      std::vector<uint32_t> k(n);
      if (fread(k.data(), 4, n, f) != n) break;
      if (!k.empty() && k[0] == kRecipeVersion && aheadKeys.emplace(k, false).second) aheadList.push_back(std::move(k));
    }
    fclose(f);
  }
  if (aheadList.size() > RECIPES_MAX) {  // keep the newest; rewrite the file without the rest
    aheadList.erase(aheadList.begin(), aheadList.end() - RECIPES_MAX);
    if (FILE* f = fopen(path.c_str(), "wb")) {
      for (const auto& k : aheadList) { uint32_t n = (uint32_t)k.size(); fwrite(&n, 4, 1, f); fwrite(k.data(), 4, n, f); }
      fclose(f);
    }
  }
  recipeFile = fopen(path.c_str(), "ab");
  host_log("[gpu] %zu pipelines known from earlier runs: compiling them ahead", aheadList.size());
}
void RecordRecipe(const std::vector<uint32_t>& k) {
  if (!recipeFile || !aheadKeys.emplace(k, false).second) return;
  uint32_t n = (uint32_t)k.size();
  fwrite(&n, 4, 1, recipeFile);
  fwrite(k.data(), 4, n, recipeFile);
  fflush(recipeFile);
}
// A few milliseconds at a time, when the command thread is idle and once per frame.
void AheadStep() {
  if (aheadPos >= aheadList.size()) return;
  if (!aheadT0) aheadT0 = host_now_ms();
  double t0 = host_now_ms();
  while (aheadPos < aheadList.size() && pipelinesPending < 8 && host_now_ms() - t0 < 4) {
    const std::vector<uint32_t>& k = aheadList[aheadPos++];
    if (pipelinesByContent.count(k)) continue;
    Recipe r;
    if (!DecodeRecipe(k, r)) continue;
    pipelinesByContent.emplace(k, BuildPipeline(r));
    aheadKeys[k] = true;
    aheadMade++;
  }
  if (aheadPos >= aheadList.size()) host_log("[gpu] compiled %llu pipelines ahead in %.1f s", (unsigned long long)aheadMade, (host_now_ms() - aheadT0) / 1000);
}

Pipeline* GetPipeline(ShaderObj* vsObj, ShaderObj* psObj, LayoutObj* layout) {
  Topology topo = TopologyOf(S.topo);
  wgpu::IndexFormat stripFmt = topo.strip && S.ib.buf ? (S.ib.fmt == 57 ? wgpu::IndexFormat::Uint16 : wgpu::IndexFormat::Uint32) : wgpu::IndexFormat::Undefined;
  uint32_t strides[16] = {};
  for (int i = 0; i < 16; i++) { BufObj* o = Buf(S.vb[i].buf); strides[i] = o && o->gpu ? S.vb[i].stride : 0; }
  if (!passInfo.tailId) {
    std::vector<uint32_t> tail;
    for (uint32_t i = 0; i < passInfo.colorCount; i++) tail.push_back((uint32_t)passInfo.rt[i]);
    tail.insert(tail.end(), { 0xffffffffu, (uint32_t)passInfo.dsFormat, passInfo.samples, (uint32_t)passInfo.roDepth | (uint32_t)passInfo.roStencil << 1 });
    auto it = internedTails.find(tail);
    if (it == internedTails.end()) it = internedTails.emplace(tail, (uint32_t)internedTails.size() + 1).first;
    passInfo.tailId = it->second;
  }
  // by the ids of the state objects: only what the pipeline depends on
  static std::vector<uint32_t> key;
  key.clear();
  key.insert(key.end(), { vsObj->hid, psObj ? psObj->hid : 0, S.layout });
  for (const LayoutSlot& sl : layout->slots) key.push_back(strides[sl.slot]);
  key.insert(key.end(), { S.topo, (uint32_t)stripFmt, S.blend.id, S.dss.id, S.rs, passInfo.tailId });
  auto it = pipelinesByIds.find(key);
  if (it != pipelinesByIds.end()) return it->second;
  // by content (the encoded recipe): an identical pipeline made for other state objects, or compiled ahead
  std::vector<uint32_t> k = { kRecipeVersion, vsObj->hashLo, vsObj->hashHi, psObj ? psObj->hashLo : 0, psObj ? psObj->hashHi : 0, psObj ? 1u : 0u, (uint32_t)vsObj->sig.size() };
  for (const SigEl& e : vsObj->sig) k.insert(k.end(), { e.sem, e.idx, e.reg, e.mask });
  k.push_back((uint32_t)layout->els.size());
  for (const LayoutEl& e : layout->els) k.insert(k.end(), { e.sem, e.idx, e.fmt, e.slot, e.offset, e.cls, e.rate });
  uint32_t used[16] = {};
  for (const LayoutSlot& sl : layout->slots) used[sl.slot] = strides[sl.slot];  // only the slots the layout reads
  k.insert(k.end(), used, used + 16);
  k.insert(k.end(), { S.topo, (uint32_t)stripFmt, passInfo.colorCount });
  for (uint32_t i = 0; i < 8; i++) k.push_back(i < passInfo.colorCount ? (uint32_t)passInfo.rt[i] : 0);
  k.insert(k.end(), { (uint32_t)passInfo.dsFormat, passInfo.samples, (uint32_t)passInfo.roDepth | (uint32_t)passInfo.roStencil << 1 });
  EncodeState(k, Get<StateObj>(S.blend.id, OK::State));
  EncodeState(k, Get<StateObj>(S.dss.id, OK::State));
  EncodeState(k, Get<StateObj>(S.rs, OK::State));
  auto ct = pipelinesByContent.find(k);
  Pipeline* pl;
  if (ct != pipelinesByContent.end()) {
    pl = ct->second;
    auto ah = aheadKeys.find(k);
    if (ah != aheadKeys.end() && ah->second) { ah->second = false; aheadUsed++; }
  } else {
    Recipe r;
    if (!DecodeRecipe(k, r)) { LogOnce("recipe", "internal: a pipeline recipe did not decode"); pl = new Pipeline(); pl->state = Pipeline::Failed; }
    else pl = BuildPipeline(r);
    pipelinesByContent.emplace(k, pl);
    RecordRecipe(k);
  }
  pipelinesByIds.emplace(key, pl);
  return pl;
}

// ---- queries -----------------------------------------------------------------------------------------------------------------------------
// Occlusion: WebGPU reports whether any sample passed, and the engine compares the count with pixel thresholds, so a query that
// passed reports a large count. Event/timestamp queries complete when the GPU has finished everything submitted so far. A
// query's result is stored into a slot in the engine's heap (`done` written last).

const uint64_t OCCLUSION_VISIBLE_COUNT = 65536;
const uint32_t OCCLUSION_CAPACITY = 2048;
wgpu::QuerySet occlusionSet;
uint32_t occlusionNext = 0;
int32_t occlusionPassSlot = -1;
QueryObj* occlusionActive = nullptr;
struct PendingResolve { uint32_t id, seq; uint64_t addr; std::vector<uint32_t> slots; };
std::vector<PendingResolve> pendingResolve;

void QueryStore(uint64_t addr, uint64_t result, uint32_t seq) {
  uint64_t base = addr / 4;
  AStore(base + 2, (int32_t)(uint32_t)result);
  AStore(base + 3, (int32_t)(uint32_t)(result >> 32));
  AStore(base, (int32_t)seq);
  Notify(base);
}
void BeginOcclusionInPass() {
  if (!occlusionActive || !hasPass || occlusionPassSlot >= 0) return;
  if (occlusionNext >= OCCLUSION_CAPACITY) { LogOnce("qfull", "occlusion query set exhausted"); return; }
  occlusionPassSlot = (int32_t)occlusionNext++;
  occlusionActive->slots.push_back((uint32_t)occlusionPassSlot);
  pass.BeginOcclusionQuery((uint32_t)occlusionPassSlot);
}
void EndOcclusionInPass() {
  if (hasPass && occlusionPassSlot >= 0) { pass.EndOcclusionQuery(); occlusionPassSlot = -1; }
}
bool IsOcclusion(const QueryObj* q) { return q->type == 1 || q->type == 5; }  // D3D11_QUERY: EVENT 0, OCCLUSION 1, TIMESTAMP 2, OCCLUSION_PREDICATE 5
void CreateQuery(uint32_t id, uint32_t type, uint64_t addr) {
  auto* q = new QueryObj();
  q->type = type; q->addr = addr;
  Put(id, q);
  if (!occlusionSet) {
    wgpu::QuerySetDescriptor d;
    d.type = wgpu::QueryType::Occlusion;
    d.count = OCCLUSION_CAPACITY;
    occlusionSet = device.CreateQuerySet(&d);
    EndPass();  // the open pass was begun without the set
  }
}
void BeginQuery(uint32_t id) {
  QueryObj* q = Get<QueryObj>(id, OK::Query);
  if (!q || !IsOcclusion(q)) return;
  q->slots.clear();
  occlusionActive = q;
  BeginOcclusionInPass();
}
void EndQuery(uint32_t id, uint32_t seq) {
  QueryObj* q = Get<QueryObj>(id, OK::Query);
  if (!q) return;
  q->seq = seq;
  if (IsOcclusion(q)) {
    if (occlusionActive == q) { EndOcclusionInPass(); occlusionActive = nullptr; }
    pendingResolve.push_back({ id, seq, q->addr, std::move(q->slots) });
    q->slots.clear();
  } else {
    pendingEvents.push_back({ q->addr, q->type, seq, (uint64_t)(host_now_ms() * 1e6) });
  }
}
bool HavePendingResolves() { return !pendingResolve.empty(); }

struct Resolves {
  struct Item { uint64_t addr; uint32_t seq, count, at; };
  std::vector<Item> items;
  wgpu::Buffer resolve, map;
  uint64_t bytes = 0;
};
// Occlusion queries ended since the last submit are resolved by commands recorded at the end of the encoder being submitted.
Resolves* RecordResolves() {
  std::vector<PendingResolve> list;
  list.swap(pendingResolve);
  auto* res = new Resolves();
  struct Run { uint32_t first, count, at; };
  std::vector<Run> runs;
  for (PendingResolve& r : list) {
    if (r.slots.empty()) { QueryStore(r.addr, 0, r.seq); continue; }
    // a query's slots are consecutive (one active query at a time, slots taken in order)
    uint32_t at = (uint32_t)res->bytes;
    runs.push_back({ r.slots[0], (uint32_t)r.slots.size(), at });
    res->items.push_back({ r.addr, r.seq, (uint32_t)r.slots.size(), at });
    res->bytes += ((uint64_t)r.slots.size() * 8 + 255) / 256 * 256;
  }
  if (occlusionNext > OCCLUSION_CAPACITY - 256 && !occlusionActive) occlusionNext = 0;  // slots are reusable once resolved
  if (!res->bytes) { delete res; return nullptr; }
  wgpu::BufferDescriptor bd;
  bd.size = res->bytes;
  bd.usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc;
  res->resolve = device.CreateBuffer(&bd);
  bd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
  res->map = device.CreateBuffer(&bd);
  for (const Run& r : runs) Enc().ResolveQuerySet(occlusionSet, r.first, r.count, res->resolve, r.at);
  Enc().CopyBufferToBuffer(res->resolve, 0, res->map, 0, res->bytes);
  return res;
}
void MapResolves(Resolves* res) {
  res->map.MapAsync(wgpu::MapMode::Read, 0, res->bytes, wgpu::CallbackMode::AllowProcessEvents, [res](wgpu::MapAsyncStatus status, wgpu::StringView) {
    if (status == wgpu::MapAsyncStatus::Success) {
      const uint64_t* v = (const uint64_t*)res->map.GetConstMappedRange(0, res->bytes);
      for (const Resolves::Item& it : res->items) {
        uint64_t sum = 0;
        for (uint32_t i = 0; i < it.count; i++) sum += v[it.at / 8 + i];
        QueryStore(it.addr, sum ? OCCLUSION_VISIBLE_COUNT : 0, it.seq);
      }
      res->map.Unmap();
    }
    res->map.Destroy();
    res->resolve.Destroy();
    delete res;
  });
}
// ---- render pass ---------------------------------------------------------------------------------------------------------------------

RtBind passRt[8];
decltype(S.ds) passDs;

bool EnsurePass() {
  if (hasPass && !memcmp(passRt, S.rt, sizeof passRt) && !memcmp(&passDs, &S.ds, sizeof passDs)) return true;
  EndPass();
  wgpu::RenderPassColorAttachment color[8];
  PassInfo info;
  uint32_t count = 0;
  bool any = false;
  for (uint32_t i = 0; i < 8; i++) {
    info.rt[i] = F::Undefined;
    TexObj* t = Tex(S.rt[i].tex);
    if (!t) continue;
    Target v = TargetView(t, S.rt[i].fmt, S.rt[i].mip, S.rt[i].slice);
    color[i].view = v.view;
    color[i].depthSlice = v.depthSlice;
    color[i].loadOp = wgpu::LoadOp::Load;
    color[i].storeOp = wgpu::StoreOp::Store;
    MarkColorDirty(t);
    info.rt[i] = v.format;
    info.samples = t->samples;
    info.width = std::max(1u, t->width >> S.rt[i].mip); info.height = std::max(1u, t->height >> S.rt[i].mip);
    any = true;
    count = i + 1;
    Use(t);
  }
  wgpu::RenderPassDescriptor desc;
  desc.colorAttachmentCount = count;
  desc.colorAttachments = color;
  if (occlusionSet) desc.occlusionQuerySet = occlusionSet;
  wgpu::RenderPassDepthStencilAttachment att;
  TexObj* dt = Tex(S.ds.tex);
  if (dt) {
    att.view = TargetView(dt, 0, S.ds.mip, S.ds.slice).view;
    bool roDepth = (S.ds.flags & 1) != 0, roStencil = (S.ds.flags & 2) != 0;
    if (roDepth) att.depthReadOnly = true; else { att.depthLoadOp = wgpu::LoadOp::Load; att.depthStoreOp = wgpu::StoreOp::Store; }
    if (HasStencil(dt->format)) { if (roStencil) att.stencilReadOnly = true; else { att.stencilLoadOp = wgpu::LoadOp::Load; att.stencilStoreOp = wgpu::StoreOp::Store; } }
    if (!roDepth && !roStencil) MarkDepthDirty(dt);
    desc.depthStencilAttachment = &att;
    info.dsFormat = dt->format;
    info.samples = dt->samples;
    info.roDepth = roDepth; info.roStencil = roStencil;
    if (!any) { info.width = std::max(1u, dt->width >> S.ds.mip); info.height = std::max(1u, dt->height >> S.ds.mip); }
    any = true;
    Use(dt);
  }
  if (!any) return false;
  info.colorCount = count;
  info.serial = ++passSerial;
  memcpy(passRt, S.rt, sizeof passRt);
  passDs = S.ds;
  pdirty = true;
  pass = Enc().BeginRenderPass(&desc);
  hasPass = true;
  statPasses++;
  passInfo = info;
  passSt = PassState();
  BeginOcclusionInPass();
  return true;
}

// refresh depth-as-float proxies before the render pass starts (a refresh is a pass of its own)
void PrepareDepthProxies(const std::vector<Binding>& bindings, int stage) {
  for (const Binding& b : bindings) {
    if (b.kind != Kind::Texture || b.binding < 32 || b.binding >= 160) continue;
    ViewObj* srv = Get<ViewObj>(S.srv[stage][b.binding - 32], OK::Srv);
    TexObj* t = srv && !srv->isBuf ? Tex(srv->res) : nullptr;
    if (!t) continue;
    if (IsDepth(t->format) && b.sample != wgpu::TextureSampleType::Depth) DepthProxy(t, b.sample == wgpu::TextureSampleType::Uint);
    else if (!IsDepth(t->format) && b.sample == wgpu::TextureSampleType::Depth) FloatToDepthProxy(t);
  }
}

// ---- bind groups -----------------------------------------------------------------------------------------------------------------------

struct BG { wgpu::BindGroup bg; bool hasDyn = false; uint32_t dyn = 0; };
struct CachedBG { wgpu::BindGroup bg; uint64_t used = 0; };  // used: the frame that last drew with it
std::unordered_map<std::vector<uint32_t>, CachedBG, KeyHash> bindGroupCache;
const size_t BG_LIMIT = 24000;
const uint64_t BG_IDLE_FRAMES = 3600, BG_SWEEP_FRAMES = 600;
uint64_t bgEvicted = 0;

wgpu::Buffer CounterBuffer(BufObj* b) {
  if (!b->counter) {
    wgpu::BufferDescriptor d;
    d.size = 16;
    d.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc | wgpu::BufferUsage::CopyDst;
    b->counter = device.CreateBuffer(&d);
  }
  return b->counter;
}
void BgUse(Obj* o) { if (o) { Use(o); o->inBG = true; } }

// The resources of one stage for `bindings` (sorted by binding). False: something required is missing and the draw is skipped.
bool StageBindGroup(int stage, Program& prog, const std::vector<Binding>& bindings, const BglRef& bgl, BG& out) {
  static std::vector<uint32_t> key;
  static std::vector<wgpu::BindGroupEntry> entries;
  key.clear();
  entries.clear();
  out = BG();
  key.insert(key.end(), { bgl.uid, prog.uid, (uint32_t)stage });
  for (const Binding& b : bindings) {
    wgpu::BindGroupEntry e;
    e.binding = b.binding;
    switch (b.kind) {
      case Kind::Uniform: {
        if (b.binding == 15) {
          if (!prog.consts) { LogOnce("c15" + prog.label, "shader %s declares binding 15 but has no constant table", prog.label.c_str()); return false; }
          e.buffer = prog.consts;
          break;
        }
        Alloc a;
        if (!PackCopy(prog, stage, a)) return false;
        e.buffer = a.chunk->buf;
        e.size = prog.pack.size;
        out.hasDyn = true;
        out.dyn = a.chunk->base + a.off;
        key.push_back(a.chunk->id);
        break;
      }
      case Kind::Sampler: case Kind::SamplerComparison: {
        uint32_t slot = (uint32_t)(b.binding - 16) & 15, sid = S.smp[stage][slot];
        StateObj* st = Get<StateObj>(sid, OK::State);
        bool cmp = b.kind == Kind::SamplerComparison;
        e.sampler = st && st->sampler && !!(st->words[0] & 0x80) == cmp ? st->sampler : DummySampler(cmp);
        key.push_back(st ? sid : 0);
        break;
      }
      case Kind::Texture: {
        uint32_t id = S.srv[stage][(uint32_t)(b.binding - 32) & 127];
        ViewObj* srv = Get<ViewObj>(id, OK::Srv);
        wgpu::TextureView v;
        if (srv && !srv->isBuf) { v = SrvView(srv, b); if (v) BgUse(Tex(srv->res)); }
        e.textureView = v ? v : DummyTexture(b);
        key.push_back(v ? id : 0);
        break;
      }
      case Kind::Storage: case Kind::ReadOnlyStorage: {
        bool isCounter = b.binding >= 176, isUav = b.binding >= 160;
        uint32_t id = isUav ? S.uav[stage][(uint32_t)(b.binding - (isCounter ? 176 : 160)) & 7] : S.srv[stage][(uint32_t)(b.binding - 32) & 127];
        ViewObj* view = Get<ViewObj>(id, isUav ? OK::Uav : OK::Srv);
        BufObj* buf = view && view->isBuf ? Buf(view->res) : nullptr;
        uint64_t resOff = 0;
        if (isCounter) e.buffer = buf ? CounterBuffer(buf) : DummyBuffer(b.binding);
        else if (buf && buf->gpu) {
          e.buffer = buf->gpu;
          uint64_t stride = buf->stride ? buf->stride : 4, off = view->a * stride;
          if (off && off % 256 == 0 && off < buf->bytes) {
            resOff = off;
            if (view->b && view->b != 0xffffffff) e.size = std::min<uint64_t>(view->b * stride, buf->bytes - off);
          } else if (off) LogOnce("bufoff" + prog.label, "%s: buffer view offset %llu is not 256-aligned; binding the whole buffer", prog.label.c_str(), (unsigned long long)off);
          e.offset = resOff;
        } else e.buffer = DummyBuffer(b.binding);
        if (buf) BgUse(buf);
        key.push_back(buf ? id : 0);
        key.push_back((uint32_t)resOff);
        break;
      }
      case Kind::StorageTexture: {
        uint32_t id = S.uav[stage][(uint32_t)(b.binding - 160) & 7];
        ViewObj* uav = Get<ViewObj>(id, OK::Uav);
        TexObj* t = uav && !uav->isBuf ? Tex(uav->res) : nullptr;
        if (!t) { LogOnce("nouavtex" + prog.label, "%s: storage texture u%d has no UAV bound", prog.label.c_str(), b.binding - 160); return false; }
        uint32_t vkey = (uint32_t)b.dim << 8 | (uint32_t)t->format;
        auto it = uav->views.find(vkey);
        if (it == uav->views.end()) {
          wgpu::TextureViewDescriptor vd;
          vd.dimension = b.dim;
          vd.baseMipLevel = std::min(uav->a, t->mips - 1);
          vd.mipLevelCount = 1;
          if (b.dim == wgpu::TextureViewDimension::e2DArray) { vd.baseArrayLayer = uav->vdim == 5 ? uav->b : 0; vd.arrayLayerCount = uav->vdim == 5 ? uav->c : t->layers; }
          else if (b.dim == wgpu::TextureViewDimension::e2D) { vd.baseArrayLayer = uav->vdim == 5 ? uav->b : 0; vd.arrayLayerCount = 1; }
          it = uav->views.emplace(vkey, ViewObj::Cached{ t->tex.CreateView(&vd), t->tex.Get() }).first;
        }
        e.textureView = it->second.view;
        BgUse(t);
        MarkColorDirty(t);
        key.push_back(id);
        break;
      }
    }
    entries.push_back(e);
  }
  auto it = bindGroupCache.find(key);
  if (it == bindGroupCache.end()) {
    // Keys contain resource ids, which are never reused: entries of destroyed textures are dead weight, so the cache is dropped when it grows large.
    if (bindGroupCache.size() > BG_LIMIT * 3 / 2) bindGroupCache.clear();
    wgpu::BindGroupDescriptor d;
    d.layout = bgl.layout;
    d.entryCount = entries.size();
    d.entries = entries.data();
    it = bindGroupCache.emplace(key, CachedBG{ device.CreateBindGroup(&d) }).first;
    bgCreated++;
  }
  it->second.used = ubo.frame;
  out.bg = it->second.bg;
  return true;
}

// Called between frames. Keys hold resource ids that are never reused, so a group whose texture or buffer is gone can never be
// hit again: anything not drawn with for a minute is dropped, a few at a time instead of the whole cache at once.
void SweepBindGroups() {
  if (ubo.frame % BG_SWEEP_FRAMES) return;
  for (auto it = bindGroupCache.begin(); it != bindGroupCache.end();) {
    if (it->second.used + BG_IDLE_FRAMES < ubo.frame) { it = bindGroupCache.erase(it); bgEvicted++; }
    else ++it;
  }
}

// ---- draw ----------------------------------------------------------------------------------------------------------------------------------

wgpu::Buffer zeroVb;
const uint64_t ZERO_VB_SIZE = 4096;
uint64_t clampedDraws = 0, indirectDraws = 0;
Pipeline* lastPl = nullptr;
uint64_t lastPlPass = 0;

void SkipDraw(const char* why) { skippedDraws++; skipReasons[why]++; }
bool IsBound(uint32_t id) {
  if (S.layout == id || S.blend.id == id || S.dss.id == id || S.rs == id || S.ib.buf == id || S.shader[0] == id || S.shader[1] == id || S.shader[2] == id) return true;
  for (const VbBind& v : S.vb) if (v.buf == id) return true;
  return false;
}
void SetBG(uint32_t index, const BG& g) {
  int64_t d = g.hasDyn ? (int64_t)g.dyn : -1;
  if (passSt.bg[index] == g.bg.Get() && passSt.dyn[index] == d) return;
  pass.SetBindGroup(index, g.bg, g.hasDyn ? 1 : 0, g.hasDyn ? &g.dyn : nullptr);
  passSt.bg[index] = g.bg.Get();
  passSt.dyn[index] = d;
}

void DoDraw(uint64_t p) {
  // kind (0 draw, 1 indexed, 2 instanced, 3 indexed instanced, 4/5 indirect), count, instanceCount, start, baseVertex, startInstance
  const uint32_t kind = *U32(p), count = *U32(p + 1), inst = *U32(p + 2), start = *U32(p + 3), startInstance = *U32(p + 5);
  const int32_t baseVertex = (int32_t)*U32(p + 4);
  ShaderObj* vsObj = Get<ShaderObj>(S.shader[0], OK::Shader);
  if (!vsObj) return SkipDraw("no vertex shader");
  ShaderObj* psObj = Get<ShaderObj>(S.shader[1], OK::Shader);
  LayoutObj* layout = Get<LayoutObj>(S.layout, OK::Layout);
  if (!layout) return SkipDraw("no input layout");
  Program* vs = ShaderProgram(vsObj);
  Program* ps = psObj ? ShaderProgram(psObj) : nullptr;
  if (!vs || (psObj && !ps)) return SkipDraw("shader without WGSL");
  PrepareDepthProxies(vs->groupBindings[0], 0);
  if (ps) PrepareDepthProxies(ps->groupBindings[1], 1);
  if (!EnsurePass()) return SkipDraw("no render target");
  Pipeline* pl;
  if (!pdirty && lastPl && lastPlPass == passInfo.serial) pl = lastPl;
  else {
    pl = GetPipeline(vsObj, psObj, layout);
    int state = pl->state;
    if (state == Pipeline::Pending) { pendingSkips++; skippedDraws++; return; }  // still compiling: looked up again by the next draw
    if (state == Pipeline::Failed) return SkipDraw("pipeline refused");
    lastPl = pl; lastPlPass = passInfo.serial; pdirty = false;
  }
  if (passSt.pl != pl->pl.Get()) { pass.SetPipeline(pl->pl); passSt.pl = pl->pl.Get(); }

  BG g0, g1;
  if (vs->hasGroup[0]) { if (!StageBindGroup(0, *vs, vs->groupBindings[0], BglFor(*vs, 0), g0)) return SkipDraw("vertex stage bindings"); SetBG(0, g0); }
  if (ps && ps->hasGroup[1]) { if (!StageBindGroup(1, *ps, ps->groupBindings[1], BglFor(*ps, 1), g1)) return SkipDraw("pixel stage bindings"); SetBG(1, g1); }

  // D3D's SV_VertexID / SV_InstanceID do not include the draw's base vertex / start instance; WebGPU's vertex_index / instance_index do.
  // Only for vertex shaders that read them are the starts folded into the vertex buffer offsets.
  const bool indexed = kind == 4 || (kind < 4 && (kind & 1));
  const bool foldV = vs->vid, foldI = vs->iid;
  const uint32_t vFirst = indexed || foldV ? 0 : start, iFirst = foldI ? 0 : startInstance;
  uint64_t vbAvail[16] = {};
  for (const LayoutSlot& e : layout->slots) {
    const VbBind& vb = S.vb[e.slot];
    BufObj* o = Buf(vb.buf);
    if (!o || !o->gpu) {  // unbound vertex slot: D3D reads zeros
      if (!zeroVb) { wgpu::BufferDescriptor d; d.size = ZERO_VB_SIZE; d.usage = wgpu::BufferUsage::Vertex; zeroVb = device.CreateBuffer(&d); }
      if (passSt.vb[e.slot] != zeroVb.Get() || passSt.vbo[e.slot] != 0) { pass.SetVertexBuffer(e.slot, zeroVb); passSt.vb[e.slot] = zeroVb.Get(); passSt.vbo[e.slot] = 0; }
      vbAvail[e.slot] = ZERO_VB_SIZE;
      continue;
    }
    int64_t shift = e.cls == 1 ? (foldI ? (int64_t)startInstance * vb.stride : 0) : (foldV ? (int64_t)(indexed ? baseVertex : (int64_t)start) * vb.stride : 0);
    int64_t signedOff = (int64_t)vb.offset + shift;
    uint64_t voff = signedOff < 0 ? 0 : (uint64_t)signedOff;
    if (voff > o->bytes) voff = o->bytes;
    if (passSt.vb[e.slot] != o->gpu.Get() || passSt.vbo[e.slot] != voff) { pass.SetVertexBuffer(e.slot, o->gpu, voff); passSt.vb[e.slot] = o->gpu.Get(); passSt.vbo[e.slot] = voff; }
    vbAvail[e.slot] = o->bytes - voff;
    Use(o);
  }
  int64_t ibMax = INT64_MAX;
  if (indexed) {
    BufObj* ib = Buf(S.ib.buf);
    if (!ib || !ib->gpu || S.ib.offset > ib->bytes) return SkipDraw("no index buffer");
    bool u16 = S.ib.fmt == 57;
    ibMax = (int64_t)((ib->bytes - S.ib.offset) / (u16 ? 2 : 4)) - start;
    if (passSt.ib != ib->gpu.Get() || passSt.ibFmt != S.ib.fmt || passSt.ibOff != S.ib.offset) {
      pass.SetIndexBuffer(ib->gpu, u16 ? wgpu::IndexFormat::Uint16 : wgpu::IndexFormat::Uint32, S.ib.offset);
      passSt.ib = ib->gpu.Get(); passSt.ibFmt = S.ib.fmt; passSt.ibOff = S.ib.offset;
    }
    Use(ib);
  }
  // dynamic state
  if (S.nViewports && (!passSt.hasVp || memcmp(passSt.vp, S.viewport, sizeof passSt.vp))) {
    const float* v = S.viewport;
    // WebGPU rejects a viewport outside the attachment; D3D clips it
    float x = std::max(0.f, v[0]), y = std::max(0.f, v[1]);
    float w = std::min((float)passInfo.width - x, v[2] - (x - v[0])), h = std::min((float)passInfo.height - y, v[3] - (y - v[1]));
    if (w > 0 && h > 0 && x < passInfo.width && y < passInfo.height) pass.SetViewport(x, y, w, h, std::min(1.f, std::max(0.f, v[4])), std::min(1.f, std::max(std::max(0.f, v[4]), v[5])));
    memcpy(passSt.vp, S.viewport, sizeof passSt.vp);
    passSt.hasVp = true;
  }
  StateObj* rs = Get<StateObj>(S.rs, OK::State);
  uint32_t sc[4] = { 0, 0, passInfo.width, passInfo.height };
  if (rs && rs->words.size() >= 8 && rs->words[7] && S.nScissors) {
    const uint32_t* c = S.scissor;
    uint32_t x0 = std::min(passInfo.width, c[0]), y0 = std::min(passInfo.height, c[1]);
    uint32_t x1 = std::min(passInfo.width, std::max(x0, c[2])), y1 = std::min(passInfo.height, std::max(y0, c[3]));
    sc[0] = x0; sc[1] = y0; sc[2] = x1 - x0; sc[3] = y1 - y0;
  }
  if (!passSt.hasSc || memcmp(passSt.sc, sc, sizeof sc)) { pass.SetScissorRect(sc[0], sc[1], sc[2], sc[3]); memcpy(passSt.sc, sc, sizeof sc); passSt.hasSc = true; }
  if (!passSt.hasBf || memcmp(passSt.bf, S.blend.factor, sizeof passSt.bf)) {
    wgpu::Color c = { S.blend.factor[0], S.blend.factor[1], S.blend.factor[2], S.blend.factor[3] };
    pass.SetBlendConstant(&c);
    memcpy(passSt.bf, S.blend.factor, sizeof passSt.bf);
    passSt.hasBf = true;
  }
  if (passSt.sref != S.dss.ref) { pass.SetStencilReference(S.dss.ref); passSt.sref = S.dss.ref; }

  if (kind >= 4) {
    BufObj* args = Buf(*U32(p + 1));
    if (!args || !args->gpu) return SkipDraw("indirect draw without an argument buffer");
    Use(args);
    if (kind == 4) pass.DrawIndexedIndirect(args->gpu, *U32(p + 2)); else pass.DrawIndirect(args->gpu, *U32(p + 2));
    indirectDraws++;
  } else {
    // D3D11 reads zeros past the end of a vertex/index buffer; WebGPU rejects the draw, and an invalid draw invalidates the whole
    // command buffer. Clamp the counts to what the bound buffers hold.
    int64_t dCount = count, dInst = inst;
    for (const VbReq& r : pl->vbReq) {
      uint64_t avail = vbAvail[r.slot];
      int64_t n = avail < r.last ? 0 : (r.stride ? (int64_t)((avail - r.last) / r.stride) + 1 : INT64_MAX);
      if (r.instance) { if (n != INT64_MAX && dInst > n - (int64_t)iFirst) dInst = n - (int64_t)iFirst; }
      else if (!indexed && n != INT64_MAX && dCount > n - (int64_t)vFirst) dCount = n - (int64_t)vFirst;
    }
    if (indexed && dCount > ibMax) dCount = std::max<int64_t>(0, ibMax);
    if (dCount != count || dInst != inst) {
      clampedDraws++;
      if (dCount <= 0 || dInst <= 0) return SkipDraw("vertex/instance/index buffer smaller than the draw");
    }
    if (indexed) pass.DrawIndexed((uint32_t)dCount, (uint32_t)dInst, start, foldV ? 0 : baseVertex, iFirst);
    else pass.Draw((uint32_t)dCount, (uint32_t)dInst, vFirst, iFirst);
  }
  drawCount++;
}

// ---- copies (GPU to GPU) -------------------------------------------------------------------------------------------------------------------

struct Sub { uint32_t mip, layer; };
Sub SubresourceOf(const TexObj* t, uint32_t sub) { return { sub % t->mips, sub / t->mips }; }
wgpu::Extent3D MipSize(const TexObj* t, uint32_t mip) {
  return { std::max(1u, t->width >> mip), std::max(1u, t->height >> mip), t->dim == 3 ? std::max(1u, t->layers >> mip) : 1u };
}
wgpu::Buffer scratchBuf, zeroSrc;
uint64_t scratchSize = 0, zeroSize = 0;

// D3D11 allows CopySubresourceRegion between a block-compressed texture and an uncompressed one whose texel size equals the block
// size (the GPU block compressor of the head blend). WebGPU refuses such texture-to-texture copies; the bytes go through a buffer.
void CopyThroughBuffer(TexObj* s, Sub ss, TexObj* d, Sub ds, uint32_t w, uint32_t h, bool hasBox, uint32_t l, uint32_t t, uint32_t dx, uint32_t dy) {
  uint32_t sb = BlockBytes(s->format), db = BlockBytes(d->format), bytes = sb ? sb : db;
  uint32_t cw = sb ? (w + 3) / 4 : w, ch = sb ? (h + 3) / 4 : h;
  uint32_t bpr = (cw * bytes + 255) / 256 * 256;
  uint64_t need = (uint64_t)bpr * ch;
  if (scratchSize < need) {
    wgpu::BufferDescriptor bd;
    bd.size = scratchSize = std::max<uint64_t>(need, 1 << 16);
    bd.usage = wgpu::BufferUsage::CopySrc | wgpu::BufferUsage::CopyDst;
    scratchBuf = device.CreateBuffer(&bd);
  }
  wgpu::TexelCopyTextureInfo src, dst;
  src.texture = s->tex; src.mipLevel = ss.mip; src.origin = { hasBox ? l : 0, hasBox ? t : 0, ss.layer };
  dst.texture = d->tex; dst.mipLevel = ds.mip; dst.origin = { dx, dy, ds.layer };
  wgpu::TexelCopyBufferInfo buf;
  buf.buffer = scratchBuf; buf.layout.bytesPerRow = bpr; buf.layout.rowsPerImage = ch;
  wgpu::Extent3D se = { sb ? cw * 4 : cw, sb ? ch * 4 : ch, 1 }, de = { db ? cw * 4 : cw, db ? ch * 4 : ch, 1 };
  Enc().CopyTextureToBuffer(&src, &buf, &se);
  Enc().CopyBufferToTexture(&buf, &dst, &de);
}

void CopyRegion(uint32_t dstId, uint32_t dstSub, uint32_t dx, uint32_t dy, uint32_t dz, uint32_t srcId, uint32_t srcSub, bool hasBox,
                uint32_t l, uint32_t t, uint32_t f, uint32_t r, uint32_t b, uint32_t bk) {
  Obj* so = GetAny(srcId), *dobj = GetAny(dstId);
  if (!so || !dobj) return;
  EndPass();
  if (so->kind == OK::Buffer && dobj->kind == OK::Buffer) {
    BufObj* s = static_cast<BufObj*>(so), *d = static_cast<BufObj*>(dobj);
    if (!s->gpu || !d->gpu) return;
    uint64_t srcOff = hasBox ? l : 0, size = (hasBox ? r - l : std::min(s->size, d->size)) & ~3u;
    if (srcOff + size > s->bytes || (uint64_t)dx + size > d->bytes || (dx & 3) || (srcOff & 3) || !size) return;
    Enc().CopyBufferToBuffer(s->gpu, srcOff, d->gpu, dx, size);
    Use(s); Use(d);
    return;
  }
  if (so->kind != OK::Texture || dobj->kind != OK::Texture) return;
  TexObj* s = static_cast<TexObj*>(so), *d = static_cast<TexObj*>(dobj);
  Sub ss = SubresourceOf(s, srcSub), ds = SubresourceOf(d, dstSub);
  wgpu::Extent3D m = MipSize(s, ss.mip);
  uint32_t w = hasBox ? r - l : m.width, h = hasBox ? b - t : m.height, dep = hasBox ? bk - f : m.depthOrArrayLayers;
  bool three = s->dim == 3;
  uint32_t sbb = BlockBytes(s->format), dbb = BlockBytes(d->format);
  if (s->format != d->format && !three && d->dim != 3 && (sbb != 0) != (dbb != 0) && (sbb ? sbb : dbb) == (sbb ? TexelBytes(d->format) : TexelBytes(s->format))) {
    CopyThroughBuffer(s, ss, d, ds, w, h, hasBox, l, t, dx, dy);
  } else {
    wgpu::TexelCopyTextureInfo src, dst;
    src.texture = s->tex; src.mipLevel = ss.mip; src.origin = { hasBox ? l : 0, hasBox ? t : 0, three ? (hasBox ? f : 0) : ss.layer };
    dst.texture = d->tex; dst.mipLevel = ds.mip; dst.origin = { dx, dy, d->dim == 3 ? dz : ds.layer };
    wgpu::Extent3D ext = { w, h, three ? dep : 1 };
    Enc().CopyTextureToTexture(&src, &dst, &ext);
  }
  MarkDepthDirty(d); MarkColorDirty(d);
  Use(s); Use(d);
}

void CopyResource(uint32_t dstId, uint32_t srcId) {
  Obj* so = GetAny(srcId), *dobj = GetAny(dstId);
  if (!so || !dobj) return;
  if (so->kind == OK::Buffer) { CopyRegion(dstId, 0, 0, 0, 0, srcId, 0, false, 0, 0, 0, 0, 0, 0); return; }
  if (so->kind != OK::Texture || dobj->kind != OK::Texture) return;
  TexObj* s = static_cast<TexObj*>(so), *d = static_cast<TexObj*>(dobj);
  EndPass();
  uint32_t mips = std::min(s->mips, d->mips);
  for (uint32_t mip = 0; mip < mips; mip++) {
    wgpu::Extent3D m = MipSize(s, mip);
    wgpu::TexelCopyTextureInfo src, dst;
    src.texture = s->tex; src.mipLevel = mip;
    dst.texture = d->tex; dst.mipLevel = mip;
    wgpu::Extent3D ext = { m.width, m.height, s->dim == 3 ? m.depthOrArrayLayers : s->layers };
    Enc().CopyTextureToTexture(&src, &dst, &ext);
  }
  MarkDepthDirty(d); MarkColorDirty(d);
  Use(s); Use(d);
}

void Resolve(uint32_t dstId, uint32_t dstSub, uint32_t srcId, uint32_t srcSub) {
  TexObj* s = Tex(srcId), *d = Tex(dstId);
  if (!s || !d) return;
  EndPass();
  Sub ss = SubresourceOf(s, srcSub), ds = SubresourceOf(d, dstSub);
  wgpu::RenderPassColorAttachment ca;
  ca.view = LevelView(s->tex, ss.mip, ss.layer);
  ca.resolveTarget = LevelView(d->tex, ds.mip, ds.layer);
  ca.loadOp = wgpu::LoadOp::Load; ca.storeOp = wgpu::StoreOp::Discard;
  wgpu::RenderPassDescriptor rp;
  rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
  Enc().BeginRenderPass(&rp).End();
  MarkColorDirty(d);
  Use(s); Use(d);
}

// ---- UAV counters (append/consume/counter buffers): one small storage buffer per D3D buffer ------------------------------------------------

const uint32_t COUNTER_SLOTS = 1024;
wgpu::Buffer counterSrc;
void SetUavCounter(uint32_t uavId, uint32_t value) {
  ViewObj* uav = Get<ViewObj>(uavId, OK::Uav);
  BufObj* buf = uav && uav->isBuf ? Buf(uav->res) : nullptr;
  if (!buf) return;
  if (!counterSrc) {
    wgpu::BufferDescriptor d;
    d.size = COUNTER_SLOTS * 4;
    d.usage = wgpu::BufferUsage::CopySrc | wgpu::BufferUsage::CopyDst;
    counterSrc = device.CreateBuffer(&d);
  }
  if (counterSlot >= COUNTER_SLOTS) Flush("uav counter");
  uint32_t slot = counterSlot++;
  QWriteBuffer(counterSrc, slot * 4, &value, 4);
  EndPass();
  Enc().CopyBufferToBuffer(counterSrc, slot * 4, CounterBuffer(buf), 0, 4);
  Use(buf);
}
void CopyCounter(uint32_t dstId, uint32_t offset, uint32_t uavId) {
  BufObj* dst = Buf(dstId);
  ViewObj* uav = Get<ViewObj>(uavId, OK::Uav);
  BufObj* src = uav && uav->isBuf ? Buf(uav->res) : nullptr;
  if (!dst || !dst->gpu || !src || (offset & 3) || (uint64_t)offset + 4 > dst->bytes) return;
  EndPass();
  Enc().CopyBufferToBuffer(CounterBuffer(src), 0, dst->gpu, offset, 4);
  Use(dst); Use(src);
}

// ---- compute -----------------------------------------------------------------------------------------------------------------------------

struct ComputePipeline {
  wgpu::ComputePipeline pl;
  BglRef bgl;
  std::vector<Binding> bindings;
};
std::unordered_map<std::string, ComputePipeline> computeCache;

ComputePipeline* ComputePipelineFor(ShaderObj* cs, Program& prog) {
  // the WGSL declares typed storage textures with a format guessed from the component count; the pipeline needs the real one
  struct Subst { int binding; std::string from; F to; };
  std::vector<Subst> subs;
  std::string key = cs->hash;
  for (const Binding& b : prog.groupBindings[0]) {
    if (b.kind != Kind::StorageTexture) continue;
    ViewObj* uav = Get<ViewObj>(S.uav[2][(uint32_t)(b.binding - 160) & 7], OK::Uav);
    TexObj* t = uav && !uav->isBuf ? Tex(uav->res) : nullptr;
    if (t && t->format != b.format) { subs.push_back({ b.binding, b.formatName, t->format }); key += "|" + std::to_string(b.binding) + ":" + std::to_string((uint32_t)t->format); }
  }
  auto it = computeCache.find(key);
  if (it != computeCache.end()) return &it->second;
  ComputePipeline cp;
  wgpu::ShaderModule module = prog.module;
  cp.bindings = prog.groupBindings[0];
  if (!subs.empty()) {
    static const struct { F f; const char* name; } names[] = { { F::RGBA8Unorm, "rgba8unorm" }, { F::RGBA16Float, "rgba16float" }, { F::R32Float, "r32float" },
      { F::RG32Float, "rg32float" }, { F::RGBA32Float, "rgba32float" }, { F::R16Float, "r16float" }, { F::RG16Float, "rg16float" }, { F::R8Unorm, "r8unorm" },
      { F::RG8Unorm, "rg8unorm" }, { F::BGRA8Unorm, "bgra8unorm" }, { F::RG11B10Ufloat, "rg11b10ufloat" }, { F::RGB10A2Unorm, "rgb10a2unorm" }, { F::R32Uint, "r32uint" } };
    std::string text = prog.text;
    for (const Subst& s : subs) {
      const char* to = nullptr;
      for (const auto& n : names) if (n.f == s.to) to = n.name;
      if (to) text = wgsl::ReplaceStorageFormat(text, s.binding, s.from, to);
    }
    module = MakeModule(text, prog.label + " [uav formats]");
    cp.bindings.clear();
    for (const Binding& b : wgsl::ParseBindings(text)) if (b.group == 0) cp.bindings.push_back(b);
    std::sort(cp.bindings.begin(), cp.bindings.end(), [](const Binding& a, const Binding& b) { return a.binding < b.binding; });
  }
  cp.bgl = MakeBgl(cp.bindings, 2);
  wgpu::PipelineLayoutDescriptor pld;
  pld.bindGroupLayoutCount = 1;
  pld.bindGroupLayouts = &cp.bgl.layout;
  wgpu::ComputePipelineDescriptor d;
  d.layout = device.CreatePipelineLayout(&pld);
  d.compute.module = module;
  d.compute.entryPoint = "main";
  d.label = wgpu::StringView(prog.label.data(), prog.label.size());
  bool failed = false;
  device.PushErrorScope(wgpu::ErrorFilter::Validation);
  cp.pl = device.CreateComputePipeline(&d);
  instance.WaitAny(device.PopErrorScope(wgpu::CallbackMode::WaitAnyOnly, [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView msg) {
    if (type == wgpu::ErrorType::NoError) return;
    failed = true;
    host_log("[gpu] compute pipeline failed (%s): %.*s", prog.label.c_str(), (int)std::min<size_t>(msg.length, 600), msg.data);
  }), UINT64_MAX);
  if (failed) cp.pl = nullptr;
  return &computeCache.emplace(key, std::move(cp)).first->second;
}

void DoDispatch(uint32_t x, uint32_t y, uint32_t z) {
  ShaderObj* cs = Get<ShaderObj>(S.shader[2], OK::Shader);
  if (!cs) { skippedDraws++; LogOnce("dispatch-nocs", "Dispatch with no compute shader bound"); return; }
  Program* prog = ShaderProgram(cs);
  if (!prog) { skippedDraws++; return; }
  PrepareDepthProxies(prog->groupBindings[0], 2);
  EndPass();
  ComputePipeline* cp = ComputePipelineFor(cs, *prog);
  if (!cp->pl) { skippedDraws++; return; }
  BG g0;
  if (!StageBindGroup(2, *prog, cp->bindings, cp->bgl, g0)) { skippedDraws++; LogOnce("dispatch-nobg" + prog->label, "Dispatch skipped: bind group failed for %s", prog->label.c_str()); return; }
  wgpu::ComputePassEncoder cpass = Enc().BeginComputePass();
  cpass.SetPipeline(cp->pl);
  cpass.SetBindGroup(0, g0.bg, g0.hasDyn ? 1 : 0, g0.hasDyn ? &g0.dyn : nullptr);
  cpass.DispatchWorkgroups(x, y, z);
  cpass.End();
  dispatchCount++;
}

void ClearUav(uint32_t id, const uint32_t* values) {
  ViewObj* uav = Get<ViewObj>(id, OK::Uav);
  if (!uav) return;
  const bool zero = !(values[0] | values[1] | values[2] | values[3]);
  if (uav->isBuf) {
    BufObj* buf = Buf(uav->res);
    if (!buf || !buf->gpu) return;
    EndPass();
    if (zero) Enc().ClearBuffer(buf->gpu);
    else {
      Flush("uav clear");
      std::vector<uint32_t> a(buf->bytes / 4);
      for (size_t i = 0; i < a.size(); i++) a[i] = values[i & 3];
      QWriteBuffer(buf->gpu, 0, a.data(), a.size() * 4);
    }
    Use(buf);
    return;
  }
  TexObj* t = Tex(uav->res);
  if (!t) return;
  uint32_t bpt = TexelBytes(t->format);
  if (!bpt) { LogOnce("clearuav" + std::to_string((uint32_t)t->format), "ClearUnorderedAccessView: texture format %u not supported", (uint32_t)t->format); return; }
  uint32_t mip = std::min(uav->a, t->mips - 1), w = std::max(1u, t->width >> mip), h = std::max(1u, t->height >> mip);
  uint32_t layers = t->dim == 3 ? std::max(1u, t->layers >> mip) : t->layers;
  wgpu::TexelCopyTextureInfo dst;
  dst.texture = t->tex; dst.mipLevel = mip;
  wgpu::Extent3D ext = { w, h, 1 };
  if (zero) {
    // a copy from a buffer that stays zero, recorded in order (no submit)
    uint32_t bpr = (w * bpt + 255) / 256 * 256;
    uint64_t need = (uint64_t)bpr * h;
    if (zeroSize < need) {
      wgpu::BufferDescriptor bd;
      bd.size = zeroSize = std::max<uint64_t>(need, 1 << 20);
      bd.usage = wgpu::BufferUsage::CopySrc;
      zeroSrc = device.CreateBuffer(&bd);
    }
    EndPass();
    wgpu::TexelCopyBufferInfo src;
    src.buffer = zeroSrc; src.layout.bytesPerRow = bpr; src.layout.rowsPerImage = h;
    for (uint32_t l = 0; l < layers; l++) { dst.origin = { 0, 0, l }; Enc().CopyBufferToTexture(&src, &dst, &ext); }
    Use(t); MarkColorDirty(t);
    return;
  }
  Flush("uav clear");
  std::vector<uint8_t> data((size_t)w * h * bpt);
  for (size_t i = 0; i < (size_t)w * h; i++) for (uint32_t c = 0; c < bpt / 4; c++) memcpy(&data[i * bpt + c * 4], &values[c & 3], 4);
  wgpu::TexelCopyBufferLayout layout;
  layout.bytesPerRow = w * bpt; layout.rowsPerImage = h;
  for (uint32_t l = 0; l < layers; l++) { dst.origin = { 0, 0, l }; QWriteTexture(dst, data, layout, ext); }
  MarkColorDirty(t);
}

// ---- mip generation, readback, clears ---------------------------------------------------------------------------------------------------------

std::unordered_map<uint32_t, wgpu::RenderPipeline> mipPipelines;
wgpu::Sampler mipSampler;

void GenerateMips(uint32_t srvId) {
  ViewObj* srv = Get<ViewObj>(srvId, OK::Srv);
  TexObj* t = srv ? Tex(srv->res) : nullptr;
  if (!t || t->mips < 2) return;
  if (IsDepth(t->format) || IsBC(t->format) || t->dim == 3 || !t->renderable) { LogOnce("mips" + std::to_string((uint32_t)t->format), "GenerateMips: unsupported texture format/dimension"); return; }
  EndPass();
  wgpu::RenderPipeline& pl = mipPipelines[(uint32_t)t->format];
  if (!pl) pl = SimplePipeline(std::string("@group(0) @binding(0) var src: texture_2d<f32>;\n@group(0) @binding(1) var smp: sampler;\n") + kFullscreenVs +
    "@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {\n  let dst = max(vec2f(1.0), floor(vec2f(textureDimensions(src, 0)) * 0.5));\n  return textureSampleLevel(src, smp, pos.xy / dst, 0.0);\n}", t->format, false);
  if (!mipSampler) {
    wgpu::SamplerDescriptor sd;
    sd.magFilter = sd.minFilter = wgpu::FilterMode::Linear;
    sd.addressModeU = sd.addressModeV = wgpu::AddressMode::ClampToEdge;
    mipSampler = device.CreateSampler(&sd);
  }
  if (t->mipPasses.empty())
    for (uint32_t layer = 0; layer < t->layers; layer++) for (uint32_t mip = 1; mip < t->mips; mip++)
      t->mipPasses.push_back({ LevelView(t->tex, mip, layer), OneTextureGroup(pl, LevelView(t->tex, mip - 1, layer), &mipSampler) });
  for (const ProxyPass& r : t->mipPasses) {
    wgpu::RenderPassColorAttachment ca;
    ca.view = r.dst; ca.loadOp = wgpu::LoadOp::Clear; ca.storeOp = wgpu::StoreOp::Store;
    wgpu::RenderPassDescriptor rp;
    rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
    wgpu::RenderPassEncoder p = Enc().BeginRenderPass(&rp);
    p.SetPipeline(pl); p.SetBindGroup(0, r.bg); p.Draw(3); p.End();
  }
  Use(t); MarkColorDirty(t);
}

// GPU resource -> the CPU storage of a D3D staging resource, written straight into the engine's heap. The copy is recorded after the
// commands that produce the data and submitted at once; only the mapping is awaited, without blocking the command loop. The
// engine waits on `flag` when it needs the bytes.
void ReadbackDone(uint64_t flag) {
  if (!flag) return;
  AStore(flag / 4, 0);
  Notify(flag / 4);
}
void Readback(uint64_t p) {
  Obj* so = GetAny(*U32(p));
  const uint32_t sub = *U32(p + 1);
  const bool hasBox = *U32(p + 2) != 0;
  const uint32_t l = *U32(p + 3), t = *U32(p + 4), f = *U32(p + 5), r = *U32(p + 6), b = *U32(p + 7), bk = *U32(p + 8);
  const uint64_t addr = *U32(p + 9) | (uint64_t)*U32(p + 10) << 32, flag = *U32(p + 15) | (uint64_t)*U32(p + 16) << 32;
  const uint32_t rowPitch = *U32(p + 11), slicePitch = *U32(p + 12), isBuf = *U32(p + 13), size = *U32(p + 14);
  if (!so) return ReadbackDone(flag);
  wgpu::BufferDescriptor bd;
  bd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
  if (isBuf) {
    BufObj* src = so->kind == OK::Buffer ? static_cast<BufObj*>(so) : nullptr;
    if (!src || !src->gpu) return ReadbackDone(flag);
    const uint64_t from = hasBox ? l : 0, n = hasBox ? r - l : std::min(size, src->size);
    const uint64_t padded = std::max<uint64_t>(4, (n + 3) & ~3ull);
    if ((from & 3) || from + padded > src->bytes) return ReadbackDone(flag);
    bd.size = padded;
    wgpu::Buffer rb = device.CreateBuffer(&bd);
    EndPass();
    Enc().CopyBufferToBuffer(src->gpu, from, rb, 0, padded);
    Flush("readback");
    Q([rb, addr, n, padded, flag] {
      rb.MapAsync(wgpu::MapMode::Read, 0, padded, wgpu::CallbackMode::AllowProcessEvents, [rb, addr, n, padded, flag](wgpu::MapAsyncStatus status, wgpu::StringView) {
        if (status == wgpu::MapAsyncStatus::Success) memcpy(M + addr, rb.GetConstMappedRange(0, padded), n);
        rb.Destroy();
        ReadbackDone(flag);
      });
    });
    return;
  }
  TexObj* src = so->kind == OK::Texture ? static_cast<TexObj*>(so) : nullptr;
  if (!src || src->samples > 1 || IsDepth(src->format)) { LogOnce("rbfmt", "readback of a multisampled or depth texture is not supported"); return ReadbackDone(flag); }
  const uint32_t mip = sub % src->mips, layer = sub / src->mips;
  wgpu::Extent3D m = MipSize(src, mip);
  const bool three = src->dim == 3;
  const uint32_t block = BlockBytes(src->format), bw = block ? 4 : 1, bytes = block ? block : (TexelBytes(src->format) ? TexelBytes(src->format) : 4);
  const uint32_t x0 = hasBox ? l : 0, y0 = hasBox ? t : 0, z0 = hasBox ? f : 0;
  const uint32_t w = hasBox ? r - l : m.width, h = hasBox ? b - t : m.height, d = hasBox ? bk - f : m.depthOrArrayLayers;
  const uint32_t bx = (w + bw - 1) / bw, by = (h + bw - 1) / bw, rowBytes = bx * bytes, bpr = (rowBytes + 255) / 256 * 256;
  const uint64_t rbSize = (uint64_t)bpr * by * d;
  if (!rbSize) return ReadbackDone(flag);
  bd.size = rbSize;
  wgpu::Buffer rb = device.CreateBuffer(&bd);
  EndPass();
  wgpu::TexelCopyTextureInfo from;
  from.texture = src->tex; from.mipLevel = mip; from.origin = { x0, y0, three ? z0 : layer };
  wgpu::TexelCopyBufferInfo to;
  to.buffer = rb; to.layout.bytesPerRow = bpr; to.layout.rowsPerImage = by;
  wgpu::Extent3D ext = { bx * bw, by * bw, three ? d : 1 };
  Enc().CopyTextureToBuffer(&from, &to, &ext);
  Flush("readback");
  Q([=] {
    rb.MapAsync(wgpu::MapMode::Read, 0, rbSize, wgpu::CallbackMode::AllowProcessEvents, [=](wgpu::MapAsyncStatus status, wgpu::StringView) {
      if (status == wgpu::MapAsyncStatus::Success) {
        const uint8_t* data = (const uint8_t*)rb.GetConstMappedRange(0, rbSize);
        for (uint32_t z = 0; z < d; z++) for (uint32_t row = 0; row < by; row++)
          memcpy(M + addr + (uint64_t)z * slicePitch + (uint64_t)row * rowPitch, data + ((uint64_t)z * by + row) * bpr, rowBytes);
      }
      rb.Destroy();
      ReadbackDone(flag);
    });
  });
}

void ClearRT(uint32_t id, uint32_t mip, uint32_t slice, float r, float g, float b, float a) {
  TexObj* t = Tex(id);
  if (!t || !t->renderable) return;
  MarkColorDirty(t);
  EndPass();
  Target v = TargetView(t, 0, mip, slice);
  wgpu::RenderPassColorAttachment ca;
  ca.view = v.view; ca.depthSlice = v.depthSlice;
  ca.loadOp = wgpu::LoadOp::Clear; ca.storeOp = wgpu::StoreOp::Store;
  ca.clearValue = { r, g, b, a };
  wgpu::RenderPassDescriptor rp;
  rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
  Enc().BeginRenderPass(&rp).End();
  statClearPasses++;
  Use(t);
}

void ClearDS(uint32_t id, uint32_t mip, uint32_t slice, uint32_t flags, float depth, uint32_t stencil) {
  TexObj* t = Tex(id);
  if (!t || !IsDepth(t->format)) return;
  MarkDepthDirty(t);
  EndPass();
  wgpu::RenderPassDepthStencilAttachment att;
  att.view = TargetView(t, 0, mip, slice).view;
  att.depthLoadOp = (flags & 1) ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
  att.depthStoreOp = wgpu::StoreOp::Store;
  att.depthClearValue = std::min(1.f, std::max(0.f, depth));
  if (HasStencil(t->format)) { att.stencilLoadOp = (flags & 2) ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load; att.stencilStoreOp = wgpu::StoreOp::Store; att.stencilClearValue = stencil; }
  wgpu::RenderPassDescriptor rp;
  rp.depthStencilAttachment = &att;
  Enc().BeginRenderPass(&rp).End();
  statClearPasses++;
  Use(t);
}

// ---- present ----------------------------------------------------------------------------------------------------------------------------------

wgpu::RenderPipeline blit;
const F kSurfaceFormat = F::BGRA8Unorm;
uint32_t surfaceW = 0, surfaceH = 0;  // submit thread
bool frameSlotBusy[FRAME_SLOTS] = {};  // guarded by qMu
std::condition_variable slotCv;
double fpsT0 = 0, busyMs = 0, idleMs = 0;
uint64_t fpsCount0 = 0, fpsDraw0 = 0, fpsSubmits0 = 0, fpsSkipped0 = 0, fpsErrors0 = 0, fpsPipes0 = 0, fpsBg0 = 0, fpsPending0 = 0;

// Submit thread: copies the back buffer into the window's drawable and shows it.
void ShowFrame(const wgpu::RenderPipeline& pipeline, const wgpu::BindGroup& source, uint32_t width, uint32_t height) {
  if (surfaceW != width || surfaceH != height) {
    wgpu::SurfaceConfiguration c;
    c.device = device;
    c.format = kSurfaceFormat;
    c.width = surfaceW = width; c.height = surfaceH = height;
    c.alphaMode = wgpu::CompositeAlphaMode::Opaque;
    c.presentMode = getenv("GTA_VSYNC") && !atoi(getenv("GTA_VSYNC")) ? wgpu::PresentMode::Immediate : wgpu::PresentMode::Fifo;
    surface.Configure(&c);
    host_log("[gpu] back buffer %ux%u", width, height);
  }
  wgpu::SurfaceTexture st;
  surface.GetCurrentTexture(&st);
  if (st.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal && st.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) return;
  wgpu::RenderPassColorAttachment ca;
  ca.view = st.texture.CreateView();
  ca.loadOp = wgpu::LoadOp::Clear; ca.storeOp = wgpu::StoreOp::Store;
  wgpu::RenderPassDescriptor rp;
  rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
  wgpu::CommandEncoder enc = device.CreateCommandEncoder();
  wgpu::RenderPassEncoder p = enc.BeginRenderPass(&rp);
  p.SetPipeline(pipeline); p.SetBindGroup(0, source); p.Draw(3); p.End();
  wgpu::CommandBuffer cb = enc.Finish();
  queue.Submit(1, &cb);
  surface.Present();
}

void Present(uint32_t id) {
  while (suspended.load()) usleep(50000);  // hold the frame; the engine waits on the full command ring
  TexObj* t = Tex(id);
  wgpu::BindGroup source;
  if (t && surface && t->samples == 1 && t->dim == 2) {
    if (!blit) blit = SimplePipeline(std::string("@group(0) @binding(0) var src: texture_2d<f32>;\n") + kFullscreenVs +
      "@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f { return textureLoad(src, vec2i(pos.xy), 0); }", kSurfaceFormat, false);
    if (!t->blitBG) t->blitBG = OneTextureGroup(blit, LevelView(t->tex, 0, 0));
    source = t->blitBG;
  }
  presentCount++;
  AheadStep();
  double now = host_now_ms();
  if (!fpsT0) { fpsT0 = now; fpsCount0 = presentCount; fpsDraw0 = drawCount; }
  else if (now - fpsT0 >= 10000) {
    uint64_t fr = std::max<uint64_t>(1, presentCount - fpsCount0);
    host_log("[gpu] fps: %.1f (%llu frames, %llu draws/frame, %.1f submits/frame), skipped draws +%llu (%llu waiting for a compiling pipeline, %d compiling now), validation errors +%llu, pipelines +%llu (%.0f ms), bind groups +%llu (cache %zu), "
             "shader modules %u (%.0f ms), command thread busy %.0f%% (%.1f ms/frame), textures %llu MB in %u, buffers %llu MB",
             fr * 1000.0 / (now - fpsT0), (unsigned long long)fr, (unsigned long long)((drawCount - fpsDraw0) / fr), (double)(submits - fpsSubmits0) / fr,
             (unsigned long long)(skippedDraws - fpsSkipped0), (unsigned long long)(pendingSkips - fpsPending0), pipelinesPending.load(), (unsigned long long)(errorCount - fpsErrors0), (unsigned long long)(pipelineCount - fpsPipes0), pipeMs,
             (unsigned long long)(bgCreated - fpsBg0), bindGroupCache.size(), shaderModuleCount, moduleMs, busyMs * 100 / std::max(1.0, busyMs + idleMs), busyMs / fr,
             (unsigned long long)(texBytesAlive >> 20), texCount, (unsigned long long)(bufBytesAlive >> 20));
    host_log("[gpu] bind groups evicted so far: %llu", (unsigned long long)bgEvicted);
    host_log_memory();
    {
      std::string why;
      for (auto& kv : flushReasons) { char b[64]; snprintf(b, sizeof b, "%s %.1f, ", kv.first.c_str(), (double)kv.second / fr); why += b; }
      host_log("[gpu] per frame: %.1f render passes + %.1f depth-proxy passes + %.1f clear passes; submits: %s| waits: submit queue %.1f ms, ring slot %.1f ms; %.1f readbacks, %.1f fences",
               (double)statPasses / fr, (double)statProxyPasses / fr, (double)statClearPasses / fr, why.c_str(), statFenceMs / fr, statSlotMs / fr,
               (double)statReadbacks / fr, (double)statFences / fr);
      flushReasons.clear();
      statPasses = statProxyPasses = statClearPasses = statFences = statReadbacks = 0;
      statFenceMs = statSlotMs = statReadbackMs = statPresentMs = 0;
    }
    if (aheadMade) host_log("[gpu] pipelines compiled ahead: %llu, of which drawn with so far: %llu; %zu to go", (unsigned long long)aheadMade, (unsigned long long)aheadUsed, aheadList.size() - aheadPos);
    if (!skipReasons.empty() && skippedDraws - fpsSkipped0 != pendingSkips - fpsPending0) {
      std::string why;
      for (auto& kv : skipReasons) { why += kv.first + " x" + std::to_string(kv.second) + "; "; }
      host_log("[gpu] skipped draws by reason (total): %s", why.c_str());
    }
    busyMs = idleMs = pipeMs = moduleMs = 0;
    fpsT0 = now; fpsCount0 = presentCount; fpsDraw0 = drawCount; fpsSubmits0 = submits; fpsSkipped0 = skippedDraws; fpsErrors0 = errorCount; fpsPipes0 = pipelineCount; fpsBg0 = bgCreated; fpsPending0 = pendingSkips;
  }
  double tq0 = host_now_ms();
  Flush("present");
  statFenceMs += host_now_ms() - tq0;
  // the uniform ring slot about to be reused must be idle: the GPU is done with the frame that last used it
  const int done = ubo.slot;
  const uint32_t width = t ? t->width : 0, height = t ? t->height : 0;
  { std::lock_guard<std::mutex> lock(qMu); frameSlotBusy[done] = true; }
  Q([pipeline = blit, source, width, height, done] {  // by value: `blit` belongs to the command thread
    if (source) ShowFrame(pipeline, source, width, height);
    queue.OnSubmittedWorkDone(wgpu::CallbackMode::AllowProcessEvents, [done](wgpu::QueueWorkDoneStatus, wgpu::StringView) {
      { std::lock_guard<std::mutex> lock(qMu); frameSlotBusy[done] = false; }
      slotCv.notify_all();
    });
  });
  ubo.slot = (ubo.slot + 1) % FRAME_SLOTS; ubo.chunk = 0; ubo.off = 0; ubo.frame++;
  SweepBindGroups();
  double ts0 = host_now_ms();
  cmdStage = "waiting for the GPU to finish an earlier frame (uniform ring slot)";
  { std::unique_lock<std::mutex> lock(qMu); slotCv.wait(lock, [] { return !frameSlotBusy[ubo.slot]; }); }
  cmdStage = "running commands";
  statSlotMs += host_now_ms() - ts0;
}

// ---- command dispatcher ---------------------------------------------------------------------------------------------------------------------

enum { RING_DATA = 64 };

void DestroyObject(uint32_t id) {
  if (IsBound(id)) pdirty = true;
  Obj* o = GetAny(id);
  if (!o) return;
  if (o->inBG) gEpoch++;
  if (Used(o)) Flush("destroy");
  if (o->kind == OK::Texture) {
    TexObj* t = static_cast<TexObj*>(o);
    std::vector<wgpu::Texture> dead = { t->tex };
    for (auto* px : { t->proxyDepth.get(), t->proxyStencil.get(), t->proxyFDepth.get() }) if (px) dead.push_back(px->tex);
    Q([dead] { for (const wgpu::Texture& x : dead) x.Destroy(); });  // after the submits that still use it
    texBytesAlive -= t->bytes; texCount--;
  } else if (o->kind == OK::Buffer) {
    BufObj* b = static_cast<BufObj*>(o);
    if (b->gpu) bufBytesAlive -= b->bytes;
    Q([gpu = b->gpu, counter = b->counter] { if (gpu) gpu.Destroy(); if (counter) counter.Destroy(); });
  } else if (o->kind == OK::Query && occlusionActive == o) {
    EndOcclusionInPass();
    occlusionActive = nullptr;
  }
  if (lastPl) { lastPl = nullptr; pdirty = true; }
  objs[id].reset();
}

uint32_t RunCommands(uint32_t tail, uint32_t head) {
  uint32_t executed = 0, sinceYield = 0;
  while (tail != head) {
    const uint64_t base = ringW + (tail >> 2), p = base + 2;
    const uint32_t op = *U32(base), n = *U32(base + 1);
    if (op == 1 /* WRAP */) { tail = RING_DATA; continue; }
    cmdLastOp.store(op, std::memory_order_relaxed);
    #define A(i) (*U32(p + (i)))
    #define FA(i) (*F32(p + (i)))
    switch (op) {
      case 0 /* NOP */: case 2 /* INIT */: break;
      case 3 /* CREATE_TEXTURE */: CreateTexture(A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7), A(8), A(9)); break;
      case 4 /* DESTROY_OBJECT */: DestroyObject(A(0)); break;
      case 5 /* CLEAR_RT */: ClearRT(A(0), A(1), A(2), FA(3), FA(4), FA(5), FA(6)); break;
      case 6 /* CLEAR_DS */: ClearDS(A(0), A(1), A(2), A(3), FA(4), A(5)); break;
      case 7 /* PRESENT */: Present(A(0)); break;
      case 8 /* FENCE */: {
        Flush("fence");
        statFences++;
        const uint64_t addr = A(0) | (uint64_t)A(1) << 32;
        const int32_t value = (int32_t)A(2);
        Q([addr, value] {
          queue.OnSubmittedWorkDone(wgpu::CallbackMode::AllowProcessEvents, [addr, value](wgpu::QueueWorkDoneStatus, wgpu::StringView) {
            AStore(addr / 4, value);
            Notify(addr / 4);
          });
        });
        break;
      }
      case 9 /* SCREENSHOT */: break;
      case 10 /* LOG */: host_log("[gpu] engine: %.*s", (int)A(0), (const char*)U32(p + 1)); break;
      case 11 /* CREATE_BUFFER */: CreateBuffer(A(0), A(1), A(2), A(3), A(4)); break;
      case 12 /* UPLOAD_BUFFER */: case 47 /* UPLOAD_BUFFER_NO_OVERWRITE */: UploadBuffer(A(0), A(1), A(2), (const uint8_t*)U32(p + 3), op == 47); break;
      case 13 /* UPLOAD_TEXTURE */: UploadTexture(A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7), A(8), A(9), A(10), A(11), (const uint8_t*)U32(p + 12)); break;
      case 14 /* CREATE_SHADER */: CreateShader(p); break;
      case 15 /* CREATE_LAYOUT */: CreateLayout(p); break;
      case 16 /* CREATE_STATE */: CreateState(p, n); break;
      case 17 /* CREATE_SRV */: CreateView(p, OK::Srv); break;
      case 37 /* CREATE_UAV */: CreateView(p, OK::Uav); break;
      case 18 /* SET_SHADER */: if (A(0) < 3 && S.shader[A(0)] != A(1)) { S.shader[A(0)] = A(1); pdirty = true; } break;
      case 19 /* SET_INPUT_LAYOUT */: if (S.layout != A(0)) { S.layout = A(0); pdirty = true; } break;
      case 20 /* SET_TOPOLOGY */: if (S.topo != A(0)) { S.topo = A(0); pdirty = true; } break;
      case 21 /* SET_VERTEX_BUFFERS */: {
        uint32_t s = A(0), c = A(1);
        for (uint32_t i = 0; i < c && s + i < 16; i++) {
          VbBind& v = S.vb[s + i];
          uint32_t nb = A(2 + i * 3), ns = A(3 + i * 3);
          if (v.stride != ns || !v.buf != !nb) pdirty = true;
          v.buf = nb; v.stride = ns; v.offset = A(4 + i * 3);
        }
        break;
      }
      case 22 /* SET_INDEX_BUFFER */: if (S.ib.fmt != A(1) || !S.ib.buf != !A(0)) pdirty = true; S.ib.buf = A(0); S.ib.fmt = A(1); S.ib.offset = A(2); break;
      case 23 /* SET_CBUFFERS */: {
        uint32_t st = A(0), s = A(1), c = A(2);
        if (st > 2) break;
        bindEpoch[st]++;
        for (uint32_t i = 0; i < c && s + i < 16; i++) S.cb[st][s + i] = { A(3 + i * 3), A(4 + i * 3), A(5 + i * 3) };
        break;
      }
      case 24 /* SET_SRVS */: { uint32_t st = A(0), s = A(1), c = A(2); if (st > 2) break; bindEpoch[st]++; for (uint32_t i = 0; i < c && s + i < 128; i++) S.srv[st][s + i] = A(3 + i); break; }
      case 25 /* SET_SAMPLERS */: { uint32_t st = A(0), s = A(1), c = A(2); if (st > 2) break; bindEpoch[st]++; for (uint32_t i = 0; i < c && s + i < 16; i++) S.smp[st][s + i] = A(3 + i); break; }
      case 38 /* SET_UAVS */: {
        uint32_t st = A(0), s0 = A(1), c = A(2);
        if (st > 2) break;
        bindEpoch[st]++;
        for (uint32_t i = 0; i < c && s0 + i < 8; i++) {
          S.uav[st][s0 + i] = A(3 + i);
          if (A(3 + c + i) != 0xffffffff) SetUavCounter(A(3 + i), A(3 + c + i));
        }
        break;
      }
      case 26 /* SET_RENDER_TARGETS */: {
        for (uint32_t i = 0; i < 8; i++) S.rt[i] = { A(i * 4), A(1 + i * 4), A(2 + i * 4), A(3 + i * 4) };
        S.ds.tex = A(32); S.ds.mip = A(33); S.ds.slice = A(34); S.ds.fmt = A(35); S.ds.flags = A(36);
        pdirty = true;
        break;
      }
      case 27 /* SET_BLEND */: if (S.blend.id != A(0)) pdirty = true; S.blend.id = A(0); for (int k = 0; k < 4; k++) S.blend.factor[k] = FA(1 + k); S.blend.mask = A(5); break;
      case 28 /* SET_DEPTH_STENCIL */: if (S.dss.id != A(0)) pdirty = true; S.dss.id = A(0); S.dss.ref = A(1); break;
      case 29 /* SET_RASTER */: if (S.rs != A(0)) pdirty = true; S.rs = A(0); break;
      case 30 /* SET_VIEWPORTS */: S.nViewports = A(0); if (S.nViewports) for (int k = 0; k < 6; k++) S.viewport[k] = FA(1 + k); break;
      case 31 /* SET_SCISSORS */: S.nScissors = A(0); if (S.nScissors) for (int k = 0; k < 4; k++) S.scissor[k] = A(1 + k); break;
      case 32 /* DRAW */: DoDraw(p); break;
      case 33 /* CLEAR_STATE */: ResetState(); break;
      case 41 /* GENERATE_MIPS */: GenerateMips(A(0)); break;
      case 42 /* READBACK */: Readback(p); statReadbacks++; break;
      case 44 /* CREATE_QUERY */: CreateQuery(A(0), A(1), A(2) | (uint64_t)A(3) << 32); break;
      case 45 /* BEGIN_QUERY */: BeginQuery(A(0)); break;
      case 46 /* END_QUERY */: EndQuery(A(0), A(1)); break;
      case 43 /* COPY_COUNTER */: CopyCounter(A(0), A(1), A(2)); break;
      case 39 /* DISPATCH */: DoDispatch(A(0), A(1), A(2)); break;
      case 40 /* CLEAR_UAV */: { uint32_t v[4] = { A(2), A(3), A(4), A(5) }; ClearUav(A(0), v); break; }
      case 34 /* COPY_REGION */: CopyRegion(A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7) != 0, A(8), A(9), A(10), A(11), A(12), A(13)); break;
      case 35 /* COPY_RESOURCE */: CopyResource(A(0), A(1)); break;
      case 36 /* RESOLVE */: Resolve(A(0), A(1), A(2), A(3)); break;
      default: LogOnce("op" + std::to_string(op), "unknown opcode %u", op); break;
    }
    #undef A
    #undef FA
    executed++;
    tail += n * 4 + 8;
    if (++sinceYield >= 512) {
      sinceYield = 0;
      AAdd(ringW + 3, (int32_t)executed); executed = 0;
      AStore(ringW + 1, (int32_t)tail);  // let the producers reuse the ring space already consumed
      Notify(ringW + 1);
    }
  }
  if (executed) AAdd(ringW + 3, (int32_t)executed);
  return tail;
}

std::unordered_map<std::string, uint32_t> errorKinds;

bool InitDevice() {
  static const wgpu::InstanceFeatureName kInstanceFeatures[] = { wgpu::InstanceFeatureName::TimedWaitAny };
  wgpu::InstanceDescriptor idesc;
  idesc.requiredFeatureCount = 1;
  idesc.requiredFeatures = kInstanceFeatures;
  instance = wgpu::CreateInstance(&idesc);
  if (void* layer = host_window_layer()) {
    wgpu::SurfaceSourceMetalLayer src;
    src.layer = layer;
    wgpu::SurfaceDescriptor sd;
    sd.nextInChain = &src;
    surface = instance.CreateSurface(&sd);
  }
  wgpu::RequestAdapterOptions opts;
  opts.powerPreference = wgpu::PowerPreference::HighPerformance;
  opts.compatibleSurface = surface;
  instance.WaitAny(instance.RequestAdapter(&opts, wgpu::CallbackMode::WaitAnyOnly,
                     [](wgpu::RequestAdapterStatus, wgpu::Adapter a, wgpu::StringView) { adapter = std::move(a); }), UINT64_MAX);
  if (!adapter) { host_log("[gpu] no WebGPU adapter"); return false; }
  const wgpu::FeatureName want[] = { wgpu::FeatureName::TextureCompressionBC, wgpu::FeatureName::TextureCompressionBCSliced3D,
    wgpu::FeatureName::Depth32FloatStencil8, wgpu::FeatureName::Float32Filterable, wgpu::FeatureName::IndirectFirstInstance,
    wgpu::FeatureName::RG11B10UfloatRenderable, wgpu::FeatureName::BGRA8UnormStorage, wgpu::FeatureName::DepthClipControl,
    wgpu::FeatureName::ClipDistances, wgpu::FeatureName::TextureFormatsTier1, wgpu::FeatureName::DualSourceBlending };
  std::vector<wgpu::FeatureName> features;
  for (wgpu::FeatureName f : want) if (adapter.HasFeature(f)) features.push_back(f);
  hasBC = adapter.HasFeature(wgpu::FeatureName::TextureCompressionBC);
  // two threads use the device (commands and submits): Dawn takes its device lock itself with this
  if (!adapter.HasFeature(wgpu::FeatureName::ImplicitDeviceSynchronization)) { host_log("[gpu] Dawn has no implicit device synchronization"); return false; }
  features.push_back(wgpu::FeatureName::ImplicitDeviceSynchronization);
  adapter.GetLimits(&limits);
  wgpu::DeviceDescriptor ddesc;
  ddesc.requiredFeatureCount = features.size();
  ddesc.requiredFeatures = features.data();
  ddesc.requiredLimits = &limits;
  ddesc.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView msg) {
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    errorCount++;
    AAdd(ringW + 4, 1);
    std::string key(msg.data, std::min<size_t>(msg.length, 200));
    for (char& c : key) if (c >= '0' && c <= '9') c = 'N';
    if (errorKinds[key]++ == 0 && errorKinds.size() <= 40) host_log("[gpu] validation error: %.*s", (int)std::min<size_t>(msg.length, 900), msg.data);
  });
  ddesc.SetDeviceLostCallback(wgpu::CallbackMode::AllowSpontaneous, [](const wgpu::Device&, wgpu::DeviceLostReason, wgpu::StringView msg) {
    host_log("[gpu] device lost: %.*s", (int)msg.length, msg.data);
  });
  // GTA_DAWN_TOGGLES=a,b,...: Dawn device toggles (e.g. skip_validation, disable_robustness) to measure what the checks cost
  wgpu::DawnTogglesDescriptor toggles;
  std::vector<std::string> toggleNames;
  std::vector<const char*> togglePtrs;
  if (const char* env = getenv("GTA_DAWN_TOGGLES")) {
    std::stringstream ss(env);
    for (std::string t; std::getline(ss, t, ',');) if (!t.empty()) toggleNames.push_back(t);
    for (const std::string& t : toggleNames) togglePtrs.push_back(t.c_str());
    toggles.enabledToggleCount = togglePtrs.size();
    toggles.enabledToggles = togglePtrs.data();
    ddesc.nextInChain = &toggles;
    host_log("[gpu] Dawn toggles: %s", env);
  }
  device = adapter.CreateDevice(&ddesc);
  if (!device) { host_log("[gpu] no WebGPU device"); return false; }
  queue = device.GetQueue();
  wgpu::BindGroupLayoutDescriptor empty;
  emptyBgl = device.CreateBindGroupLayout(&empty);
  wgpu::AdapterInfo info;
  adapter.GetInfo(&info);
  host_log("[gpu] device ready: %.*s, %zu of %zu wanted features%s, %s", (int)info.device.length, info.device.data, features.size() - 1, sizeof want / sizeof *want,
           hasBC ? "" : " (no BC textures)", surface ? "window" : "no window");
  return true;
}

// When no frame has been presented for a while, says once where everything is.
void* WatchdogMain(void*) {
  pthread_setname_np("gpu watchdog");
  uint64_t last = 0;
  int stalled = 0;
  bool dumped = false;
  for (;;) {
    sleep(5);
    uint64_t now = presentCount;
    static bool tested = false;
    if (!tested && now >= 600 && getenv("GTA_TEST_DUMP")) { tested = true; host_dump_threads(); }  // test aid: one dump of a healthy game
    if (suspended.load()) { last = now; stalled = 0; continue; }
    if (now != last || now < 600) { last = now; stalled = 0; continue; }  // also quiet while the game is still loading
    if (++stalled < 3 || dumped) continue;
    dumped = true;
    int pending, busy[FRAME_SLOTS];
    size_t queued;
    { std::lock_guard<std::mutex> lock(qMu); pending = qSubmitsPending; queued = qOps.size(); for (int i = 0; i < FRAME_SLOTS; i++) busy[i] = frameSlotBusy[i]; }
    host_log("[gpu] STALL: no frame presented for 15 s. command thread: %s (last opcode %u); submit thread: %s; submit queue %zu ops, %d submits pending; "
             "ring slots busy %d %d %d (current %d); pipelines compiling %d; ring head %d tail %d sleeping %d state %d; presents %llu, draws %llu",
             cmdStage.load(), cmdLastOp.load(), submitStage.load(), queued, pending, busy[0], busy[1], busy[2], ubo.slot, pipelinesPending.load(),
             ALoad(ringW), ALoad(ringW + 1), ALoad(ringW + 5), ALoad(ringW + 2), (unsigned long long)presentCount, (unsigned long long)drawCount);
    host_dump_threads();
  }
  return nullptr;
}

void* GpuMain(void*) {
  pthread_setname_np("gpu");
  const uint64_t hi = ringW, ti = hi + 1;
  uint32_t tail = RING_DATA;
  if (!InitDevice()) {
    AStore(hi + 2, 2);
    for (;;) {  // drain so the producers never block
      AStore(ti, ALoad(hi));
      Notify(ti);
      usleep(5000);
    }
  }
  LoadRecipes();
  pthread_t submitThread, watchdog;
  pthread_create(&submitThread, nullptr, SubmitMain, nullptr);
  pthread_detach(submitThread);
  pthread_create(&watchdog, nullptr, WatchdogMain, nullptr);
  pthread_detach(watchdog);
  AStore(ti, (int32_t)tail);
  AStore(hi + 2, 1);
  for (;;) {
    uint32_t head = (uint32_t)ALoad(hi);
    if (head == tail) {
      if (!pendingEvents.empty() || !pendingResolve.empty()) Flush("idle");  // nobody is producing: let waiting fences/queries finish
      AheadStep();
      double w0 = host_now_ms();
      // producers only notify while `sleeping` (header word 5) is set: set it, then re-read head so a command published in between is not missed
      AStore(hi + 5, 1);
      cmdStage = "idle: the engine has sent no commands";
      if ((uint32_t)ALoad(hi) == tail) host_futex_wait32(hi * 4, tail, 2000000);
      cmdStage = "running commands";
      AStore(hi + 5, 0);
      idleMs += host_now_ms() - w0;
      continue;
    }
    double b0 = host_now_ms();
    tail = RunCommands(tail, head);
    busyMs += host_now_ms() - b0;
    AStore(ti, (int32_t)tail);
    Notify(ti);
  }
  return nullptr;
}

}  // namespace

void gpu_suspend(int on) { suspended.store(on != 0); }

void gpu_start(uint64_t ring, uint64_t size, uint64_t dbg) {
  M = host_mem_base();
  ringW = ring / 4;
  host_log("[gpu] start: ring %llx size %llu shaders %s", (unsigned long long)ring, (unsigned long long)size, host_shader_dir());
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 16u << 20);
  pthread_t t;
  pthread_create(&t, &a, GpuMain, nullptr);
  pthread_detach(t);
}
