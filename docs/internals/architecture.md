# Architecture

For contributors and agents. Directory map: public headers in
`include/rendy/<module>/`, implementation in `src/<module>/`; Vulkan, SDL,
FreeType and Yoga never appear in public headers.

## GPU foundation (`src/gpu/`)

- **Context** (`context.*`): Vulkan 1.3 instance (volk-loaded), discrete-GPU
  pick requiring `dynamicRendering`, `synchronization2`, `timelineSemaphore`
  and descriptor-indexing features; one graphics+present queue; VMA
  allocator; a `VkPipelineCache` persisted to `$XDG_CACHE_HOME/rendy/`.
  Validation layers attach when built with `RENDY_VULKAN_VALIDATION` and the
  layer is installed.
- **Swapchain** (`swapchain.*`): BGRA8 sRGB, FIFO (vsync) or
  mailbox/immediate; recreation driven by acquire results.
- **FrameRing** (`frame.*`): 2 frames in flight. Per slot: command pool +
  buffer, deletion queue (runs when the slot's previous GPU work finished).
  One timeline semaphore carries CPU↔GPU pacing (value = frame counter);
  binary semaphores only for acquire/present. Skipped frames (minimized) do
  not advance the ring.
- **BindlessTable** (`bindless.*`): the one global descriptor set — a
  4096-entry partially-bound, update-after-bind array of combined image
  samplers. Every texture (UI images, glyph atlas pages, material maps, the
  HDR resolve target) gets a stable `u32` index; index 0 is a 1×1 white
  pixel. This is why nothing ever rebinds per draw.
- **Uploader** (`upload.*`): blocking staging-buffer upload with a one-shot
  command buffer; also generates mip chains with blits. Load-time only —
  per-frame data goes through persistently mapped buffers instead.
- **TexturePool** (`texture.*`): VkImage + view + sampler per texture,
  registered bindlessly. RGBA8 (sRGB or UNORM) and R8 formats.

No render passes exist anywhere — everything is Vulkan 1.3 dynamic
rendering, and all barriers use synchronization2.

## Frame composition (`src/app/app.cpp` present())

```
acquire → [3D: shadow passes → scene pass (HDR, MSAA) → resolve → tonemap]
        → 2D pass (loadOp = LOAD over 3D, else CLEAR) → present
```

`beginFrame` only acquires and starts the command buffer; the actual passes
are recorded in `present()`, when it's known whether `frame.draw(scene,...)`
was called. Pure-2D apps skip the 3D chain entirely.

## 2D batching (`src/canvas/`)

Every `drawRect`/`drawImage`/glyph pushes a 96-byte `Quad2D` instance into a
CPU vector (`canvas_data.hpp`). At flush, instances memcpy into a
per-frame-slot mapped SSBO and render as ONE
`vkCmdDraw(4, N)` instanced triangle strip (`renderer2d.cpp`,
`shaders/quad2d.*`):

- Vertex shader expands `gl_VertexIndex`/`gl_InstanceIndex` — no vertex
  buffer at all.
- Rounded corners, borders and edge AA come from an analytic rounded-box
  SDF; radius clamps to half-size (CSS pill behavior).
- Clipping is shader-side: a per-frame SSBO of clip rects, an index per
  instance — scissor never breaks the batch.
- Text quads sample R8 glyph-atlas pages (`src/text/glyph_cache.*`:
  FreeType raster → stb_rect_pack into 1024² pages, uploaded on change).
  Note: `stbrp_context` self-references, so atlas pages live behind
  `unique_ptr` and must never move.
- Colors are sRGB in instance data, converted to linear in the shader; the
  sRGB swapchain encodes on write, so blending is linear-correct.

## CSS engine (`src/css/`, `src/ui/`)

Pipeline: `tokenizer` → `parser` (rules with expanded shorthands; unknown
properties collected, not fatal) → `cascade` (right-to-left selector
matching, spec specificity `(id, class+pseudo, type)`, stable-sorted, then
inline styles) → `ComputedStyle` (flat struct; inherited text props seeded
from the parent, `em` resolved during application, `font-size` first).

`ui::Context` (`src/ui/context.cpp`) owns the Node tree. Each node has a
Yoga node; `applyStyleToYoga` (`yoga_layout.cpp`) maps ComputedStyle 1:1
onto Yoga (web defaults on, so stretch/shrink match browsers). Leaf nodes
with text get a Yoga measure func using the glyph cache. One dirty flag
triggers restyle + relayout; hover/active/focus changes set it. Hit testing
walks the tree back-to-front and respects overflow clips; wheel scrolling
targets the nearest scrollable ancestor. `.css` files are re-parsed on
mtime change (debug builds).

## 3D renderer (`src/scene/`)

- **Data**: `SceneImpl` — flat node array (parents by index, world matrices
  recomputed each draw), `MeshStore` (all vertices/indices in two big
  device-local buffers, bound once), `GpuMaterial` array mirrored to a
  mapped SSBO each frame, lights likewise.
- **Draw path** (`renderer3d.cpp`): transforms for every alive mesh node go
  into a per-frame SSBO (shadow casters may be off-screen); the camera pass
  draws the frustum-culled subset (sphere vs 6 planes). Push constants =
  transform index + material index; one pipeline for all opaque meshes.
- **Shadows**: fixed arrays created up front — CSM 2048²×4, spots 1024²×8,
  point cubes 512²×24 — rendered by one depth-only vertex-shader pipeline
  (light matrix in push constants), sampled via compare samplers (PCF 3×3)
  and, for cubes, a manual depth-reference compare. Cascades use practical
  splits (λ=0.75), sphere-fit + texel snapping for stability.
- **Post**: HDR RGBA16F 4×MSAA → resolve (registered bindlessly) → tonemap
  fullscreen triangle (Khronos PBR Neutral / ACES / off + exposure) into the
  swapchain.

## Shaders (`shaders/`, `cmake/CompileShaders.cmake`)

GLSL 460 compiled at build time by the FetchContent-built glslang
standalone (no Vulkan SDK needed), converted to `constexpr uint32_t[]`
headers (`cmake/Bin2H.cmake`) and embedded — binaries have zero runtime
file dependencies. `#include` works via `GL_GOOGLE_include_directive`
(`scene_common.glsl` holds the shared set-1 interface; keep it in sync with
`FrameUbo`/`GpuMaterial`/`GpuLight` in C++).

## Audio (`src/audio/`)

SDL3 audio stream (48 kHz float stereo) pulls from a 32-voice mixer in the
audio callback. Sounds are fully decoded/resampled at load (dr_wav,
stb_vorbis). Voice control is atomics-only — no locks on the audio thread;
`generation` counters make stale VoiceRefs harmless.

## Conventions worth keeping

- Errors: `Result<T>` at the public boundary, `VK_CHECK` (log + abort)
  internally, `log::warn` for degradable failures.
- Per-frame GPU data: mapped host-coherent buffers, doubled per frame slot,
  grown by power of two with old buffers retired through the deletion queue.
- Teardown order in `AppImpl::~AppImpl` is strict reverse creation order —
  everything owning GPU resources dies before the Context.
