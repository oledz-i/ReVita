# Native Direct3D 12 renderer: plan

ReVita's Vulkan renderer already draws through Direct3D 12: Dozen (Mesa's
Vulkan-on-D3D12 driver) turns every Vulkan call into a D3D12 one. A native
renderer would remove that layer. This page is the plan for building one
without breaking the two renderers people play on.

## Why it is worth doing

- Fewer layers: Dozen builds pipeline variants at draw time (the
  "draw-time variant" stalls in the logs), translates descriptor sets into
  descriptor heaps and emulates render passes. A native backend can build
  its pipelines up front and bind resources directly.
- Features Dozen cannot give Vita3K: reading the render target being drawn
  to (programmable blending), which both renderers now emulate with a copy
  before each such draw, can use a rasterizer-ordered view (ROV) on D3D12.
- No PSO serialization on the console (it removes the device), so pipeline
  caching has to be ours anyway.

## Shape

The core's renderers implement `renderer::State` and `renderer::Context`
(`vita3k/renderer`). The D3D12 backend is a third implementation, next to
`gl/` and `vulkan/`, chosen by a third value of the Backend Renderer option.

| Piece | Source |
|---|---|
| Shaders | The existing GXP to SPIR-V translator (`vita3k/shader`), then SPIRV-Cross (already in `external/`) to HLSL, then `D3DCompile` (Shader Model 5.1, DXBC; D3D12 accepts it, and `d3dcompiler_47` is available to UWP apps) |
| Pipelines | One PSO per (vertex program, fragment program, blend, depth/stencil, formats, primitive type): the same key the Vulkan backend's pipeline cache uses |
| Surfaces | Port of the Vulkan surface cache: textures and render targets in placed resources, copies with `CopyTextureRegion` |
| Textures | Port of the Vulkan texture cache; uploads through a ring of upload heaps |
| Vertex and uniform data | Ring buffers in upload heaps, as the Vulkan backend's ring buffers |
| Programmable blending | Render target as a UAV in a rasterizer-ordered view where the hardware has them, else the copy the other renderers use |
| Present | Straight into the app's own swap chain (the presenter in `D3D12Host.cpp` already owns one): no readback to the CPU, which both current renderers need |

## Order of work

1. **Done (revita-next):** the backend exists and can be picked
   (Settings, Renderer: Direct3D 12). The core asks the frontend for a
   Direct3D 12 context (libretro's `RETRO_HW_CONTEXT_D3D12`), keeps every
   guest command flowing (contexts, render targets, programs, notifications,
   sync objects) and presents the display buffer from guest memory through
   its own textures. GXM draws are accepted and dropped. Core side:
   `scripts/core-patches/0010-d3d12-renderer-stage-1.patch`
   (`vita3k/renderer/src/d3d12/`); frontend side:
   `src/Vita3K.UWP/VitaD3D12Bridge.cpp`.
2. **Done (revita-next):** shaders. Every draw builds the shaders it
   would use: GXP to SPIR-V (the Vulkan translator), SPIR-V to HLSL
   (SPIRV-Cross, shader model 5.1, Vulkan clip space flipped), HLSL to DXBC
   (the frontend's `D3DCompile`). Results are cached in memory and on disk
   (`<hash>.vs|ps.dx<version>.dxbc` in the game's shader folder); a shader
   that fails is logged once with its HLSL written to the shader log folder.
   The log prints a running count every 600 frames. Still no drawing.
   `scripts/core-patches/0011-d3d12-renderer-stage-2-shaders.patch`.
3. **First pass in (revita-next), untested on the console:** drawing.
   One render target per color surface (RGBA8 with a 32-bit depth buffer),
   one root signature for every draw (SPIR-V set N is register space N),
   pipelines from the fragment program's blending and the depth state,
   textures through the shared texture cache (BC1 to BC5 native), render
   targets sampled as textures, vertex/index/uniform data through a
   segmented upload ring. The display buffer is copied on the GPU from the
   render target drawn at its address; when none was, the picture in guest
   memory is shown as before. Fragment inputs are rebuilt in the vertex
   shader's output order so Direct3D links the two stages. Left out for
   now: stencil, culling, the mask bit, programmable blending, depth-only
   scenes, surface sync back to guest memory.
   `scripts/core-patches/0012-d3d12-renderer-stage-3-drawing.patch`.
4. Surface sync, programmable blending, mask bit.
5. Shader and pipeline caches on disk.

Each step ships only when the one before runs on the console, with Vulkan
and OpenGL untouched.
