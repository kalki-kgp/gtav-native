# Later: a direct Metal renderer

Status: not started. Decided on 2026-10-10 to go to the iPhone build first and come back to this.

## Why

The native build draws through Dawn (Google's WebGPU library), in `host/gpu.cc`. Measured on the M1 Air at 1280x720 it
reaches about 45 fps in busy scenes (2,400-2,600 draws per frame) and about 56 in lighter ones, while the engine alone can
produce about 97 frames per second. The renderer is the limit, and the cost is Dawn's, not the game's:

- **Everything is done twice.** Dawn records each command when it is issued (with validation), then translates the recording to
  Metal at submit. In the profile the translation alone was about 43% of the renderer's time.
- **One device lock.** With two renderer threads (one recording, one submitting) Dawn's single device lock made the recording
  thread wait about 40% of the time, at every pass begin and end.
- **The API shape is wrong for this engine.** The engine emits Direct3D 11-style commands: set a texture in slot N, set a
  constant buffer in slot N, draw. WebGPU wants immutable bind groups and pipeline layouts, so `gpu.cc` builds and caches bind
  groups (thousands, with churn while streaming) and copies every constant buffer into a uniform ring per draw.

Skipping Dawn's validation (`GTA_DAWN_TOGGLES=skip_validation`) gained only 5-10%. Chrome gets around the double work by doing
the second half in a separate GPU process, which is why this build is expected to be roughly level with the browser rather than
well ahead of it.

## The idea

Replace `host/gpu.cc` with a renderer that issues Metal calls directly. The D3D11-style command stream maps onto Metal almost
one to one:

| Engine command (D3D11 style) | Metal |
|---|---|
| Set shader resource view in slot N | `setFragmentTexture:atIndex:` / `setVertexTexture:atIndex:` |
| Set sampler in slot N | `setFragmentSamplerState:atIndex:` |
| Set constant buffer in slot N | `setFragmentBuffer:offset:atIndex:` on a per-frame ring |
| Set vertex / index buffers | `setVertexBuffer:offset:atIndex:`, the index buffer goes on the draw call |
| Blend, input layout, shaders, target formats | one `MTLRenderPipelineState`, cached by the same key `gpu.cc` uses |
| Depth-stencil state | `MTLDepthStencilState` |
| Rasterizer state | `setCullMode:`, `setFrontFacingWinding:`, `setDepthBias:...`, `setTriangleFillMode:` |
| Set render targets | a new `MTLRenderCommandEncoder` |
| Draw, draw indexed, indirect | `drawPrimitives`, `drawIndexedPrimitives`, the indirect variants |
| Dispatch | `MTLComputeCommandEncoder` |
| Copies, mip generation | `MTLBlitCommandEncoder` |

No bind groups, no bind-group cache, no layouts, and each command is encoded once.

## What it takes

1. **Shaders.** The 4,808 shaders exist as WGSL (`<mirror>/b/<build>/shaders`). Convert them to Metal Shading Language ahead of
   time with Tint (Dawn's shader compiler, which has a command-line tool) and ship the `.metal` text or compiled
   `.metallib` files. The conversion has to reproduce what `host/wgsl.cc` does at load time today:
   - constant buffers merged into one struct (`PackUniforms`), or better, left as separate buffers since Metal has no limit
     that forces the merge;
   - the alpha-test variant that stands in for alpha-to-coverage (`AddAlphaTest`);
   - vertex-shader variants with flat-interpolated outputs to match the pixel shader (`MarkFlat`);
   - storage-texture format substitution for compute shaders (`ReplaceStorageFormat`).
   Tint assigns Metal buffer/texture/sampler indices from the WGSL group and binding numbers; the renderer needs that mapping
   (Tint can emit it, or the bindings can be remapped to fixed slots when converting).
2. **The renderer.** A rewrite of `gpu.cc` (about 2,400 lines) against Metal, in Objective-C++. The command ring protocol,
   the object table, the state tracking, the pipeline key, the draw clamping, queries, readbacks and the present path carry
   over; the bind-group and uniform-ring machinery goes away. `wgpu_worker.js` in the mirror remains the reference for what
   each opcode must do.
3. **Things WebGPU forced that Metal does not need.** Depth textures read as float (the depth-proxy copies, 6-8 extra passes
   per frame) can be sampled directly. A8 textures need no expansion (`MTLPixelFormatA8Unorm`). BC textures work on the
   iPhone 17 through Metal directly, which is not true of the prebuilt Dawn.
4. **Passes.** Apple GPUs are tile-based, so every pass boundary costs a store and a load. Today there are about 80 per frame
   (52 draws passes, 6 depth-proxy, 20 standalone clears). With Metal the clears can become the load action of the next pass
   on that target, and the proxies disappear.

## Risks

- Tint's Metal output for a few shaders may need hand fixes; the WGSL itself compiles cleanly today (0 of 4,808 rejected,
  `tests/shadercheck.cc` and `tests/wgslcheck.cc`).
- Compiling thousands of Metal shaders at first use is slow; precompiled `.metallib` files or Metal's binary archives avoid
  the stall the Dawn build has on first run.
- It is several days of work before there is a picture again, and the Dawn renderer has to stay working meanwhile as the
  reference.

## Where things are

- Current renderer: `host/gpu.cc`, shader text rewriting: `host/wgsl.cc`.
- Stand-in renderer for measuring the engine alone: `host/gpu_null.c` (`GTA_GPU=null`).
- Reference implementation: `<mirror>/b/<build>/wgpu_worker.js`.
