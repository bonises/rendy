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
- **Draw path** (`renderer3d.cpp`): everything is instanced. Shadow groups
  (all alive opaque casters, keyed by mesh) and camera groups (the
  frustum-culled visible set, keyed by mesh+material) each get a contiguous
  run of world matrices in the per-frame transform SSBO and one
  `vkCmdDrawIndexed` with an instance count; shaders index
  `transforms[pushBase + gl_InstanceIndex]`. Blended nodes draw
  individually, sorted back-to-front, on a no-depth-write blend pipeline
  after the opaque pass; alpha-mask materials discard in the fragment
  shader. 20k instanced cubes render at ~270 fps with full shadows.
- **Skinning/animation**: glTF clips import as `SceneAnimation` channel
  lists sampled on the CPU (`Scene::updateAnimations`) into node transforms;
  skins store joint node indices + inverse binds, and per frame each used
  skin's joint matrices (`world * inverseBind`) upload to the joints SSBO
  (set 1 binding 7). Skinned vertices carry JOINTS_0/WEIGHTS_0
  (`Vertex::joints/weights`); `rendyModelMatrix` in `scene_common.glsl`
  blends joint matrices when `jointBase != NO_JOINTS` — the same path serves
  the camera pass and the shadow passes, so shadows are skinned too.
  Skinned draws skip instancing and frustum culling (animated bounds).
  **Morph targets**: per-target position/normal deltas live in a static
  SSBO in the MeshStore (binding 12); per-frame weights (binding 13) are
  gathered per drawn node, and both vertex shaders apply deltas before
  skinning. Weight animations sample per-element (step/linear/cubic) and
  blend through the same accumulator as TRS channels.
- **Shadows**: fixed arrays created up front — CSM 2048²×4, spots 1024²×8,
  point cubes 512²×24 — rendered by one depth-only vertex-shader pipeline
  (light matrix in push constants), sampled via compare samplers (PCF 3×3)
  and, for cubes, a manual depth-reference compare. Cascades use practical
  splits (λ=0.75), sphere-fit + texel snapping for stability.
- **Forward+ clusters**: point/spot lights bin on the CPU into a 16×9×24
  log-z cluster grid (conservative projected-AABB tile ranges; near-plane
  crossers cover the full screen). Per-frame SSBOs hold (offset,count) per
  cluster + a flat light index list (bindings 14/15); directional lights
  sort first in the light buffer and are always shaded, the fragment shader
  walks only its cluster for the rest. 1000 point lights ≈ 1900 fps release.
- **Environment/IBL** (`scene/environment.*`): `setEnvironment` bakes an
  equirect HDR once with transient graphics pipelines — env cubemap (512³,
  mipped), cosine irradiance (32³), GGX-prefiltered chain (256³, 6 mips,
  roughness per mip), split-sum BRDF LUT (512²) — bound at set 1 bindings
  8–11 (1×1 black defaults otherwise). The skybox draws as a fullscreen
  triangle at far depth inside the HDR pass; `mesh.frag` swaps flat ambient
  for irradiance + prefiltered-specular when `counts.z` is set.
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

**Hot reload** (`RENDY_SHADER_HOT_RELOAD`, ON in the debug preset): glslang
is linked into rendy (`gpu/shader_compiler.*`), the source `shaders/` dir is
mtime-polled from `pollEvents` (0.5 s), and a changed file recompiles
in-process; renderers swap the `ShaderBlob` and rebuild their pipelines with
old ones retired through the frame ring (`reloadShader` on
Renderer2D/Renderer3D). A `.glsl` include change recompiles every stage.
Compile errors log and keep the old pipeline.

## Audio (`src/audio/`)

SDL3 audio stream (48 kHz float stereo) pulls from a 32-voice mixer in the
audio callback. Sounds are fully decoded/resampled at load (dr_wav,
stb_vorbis). Per-voice parameters are relaxed atomics; voice claim/free
(`play`, `unload`) briefly takes SDL's stream lock to exclude the callback.
`generation` counters make stale VoiceRefs harmless.

## Conventions worth keeping

- Errors: `Result<T>` at the public boundary, `VK_CHECK` (log + abort)
  internally, `log::warn` for degradable failures.
- Per-frame GPU data: mapped host-coherent buffers, doubled per frame slot,
  grown by power of two with old buffers retired through the deletion queue.
- Teardown order in `AppImpl::~AppImpl` is strict reverse creation order —
  everything owning GPU resources dies before the Context.
