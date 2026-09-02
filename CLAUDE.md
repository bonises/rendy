# rendy

C++20 rendering library on Vulkan 1.3: batched 2D/UI with a real CSS subset
(flexbox via Yoga), forward-rendered PBR 3D with shadows, glTF loading, and
audio via SDL3. Linux/X11 first. Namespace `rendy::`.

## Build & test

```sh
cmake --preset debug          # first run downloads all deps (FetchContent)
cmake --build --preset debug
ctest --preset debug
./build/debug/examples/01_hello/01_hello
```

Presets: `debug` (validation layers on), `release`, `asan`. Options:
`RENDY_BUILD_EXAMPLES`, `RENDY_BUILD_TESTS`, `RENDY_WERROR`,
`RENDY_VULKAN_VALIDATION`, `RENDY_SHADER_HOT_RELOAD` (ON in the debug
preset: edit `shaders/*` while an app runs and pipelines rebuild live),
`RENDY_SANITIZE`, `RENDY_INSTALL` (`cmake --install` ships a
self-contained librendy.a + find_package config; cmake/Install.cmake).

CI (`.github/workflows/ci.yml`) builds GCC + Clang × debug/release plus a
GCC asan job, all with `RENDY_WERROR=ON` — keep the tree warning-clean
under both compilers. Tests are CPU-only; CI has no GPU.

GPU smoke tests (local only): configure with `-DRENDY_GPU_TESTS=ON`, run
`ctest -L gpu` or `./build/debug/tests/rendy_gpu_tests`. Hidden window +
screenshot readback (`App::requestScreenshot`); covers 2D/3D rendering,
the UI damage cache and probe ownership. Run them after renderer changes.

## Where things live

| Area | Public API | Implementation | Tests |
|---|---|---|---|
| Errors/log/handles/color | `include/rendy/core/` | `src/core/` | `tests/core/` |
| Math (GLM aliases) | `include/rendy/math/` | header-only | `tests/math/` |
| Window/input/main loop | `include/rendy/app/` | `src/app/` | — |
| Vulkan internals | `include/rendy/gpu/` (opaque) | `src/gpu/` | `tests/gpu/` (opt-in) |
| Immediate 2D drawing | `include/rendy/canvas/` | `src/canvas/`, `src/text/` | `tests/text/` |
| Retained UI + CSS | `include/rendy/ui/` | `src/ui/`, `src/css/` | `tests/css/`, `tests/layout/` |
| 3D scene/lights/glTF | `include/rendy/scene/` | `src/scene/` | `tests/scene/` |
| Audio | `include/rendy/audio/` | `src/audio/` | `tests/audio/` |
| Shaders (GLSL 460) | — | `shaders/` (embedded as SPIR-V at build) | — |

Dependency pins: `cmake/Dependencies.cmake`. Shader build: `cmake/CompileShaders.cmake`.

## Conventions

- Public headers never include Vulkan/SDL/FreeType/HarfBuzz/Yoga headers — opaque
  handles and pimpl only. GLM is the one allowed public dependency (aliased
  as `rendy::Vec3` etc. in `math/math.hpp`); fmt is public for `log.hpp`.
- No exceptions across the public API: fallible ops return `rendy::Result<T>`
  (`core/result.hpp`); `rendy::err("...", args)` formats an `Error`.
- GPU-side objects are generational `Handle<Tag>`s (`core/handle.hpp`).
- Colors are sRGB floats (`core/color.hpp`); shaders convert to linear.
- Screen space is top-left origin, +y down; depth is 0..1 (GLM forced).
- Docs: `docs/` is the source of truth — `getting-started.md`,
  `guides/{2d-canvas,ui-css,3d,audio}.md`, `api/` (one page per module),
  `internals/architecture.md`. Update the matching guide + api page when
  changing a module's public surface.
- Keep `shaders/scene_common.glsl` in sync with `FrameUbo`/`GpuMaterial`/
  `GpuLight` in `src/scene/`.

## Architecture in one paragraph

One `App` owns the SDL window and the GPU context (`src/gpu/`): Vulkan 1.3
with dynamic rendering (no render passes), volk + VMA, two frames in flight,
one timeline semaphore, a per-frame linear arena for dynamic data, and one
global bindless descriptor set (u32 texture indices) shared by every pass.
Each frame a fixed FrameComposer records: uploads → shadow passes → 3D
forward pass (HDR, MSAA) → tonemap → 2D/UI on top. The 2D renderer draws
every rect/image/glyph as one instanced quad stream (clipping done in the
shader via clip-rect indices, rounded corners via SDF). The retained UI tree
computes CSS styles (`src/css/`) into Yoga layouts and paints itself through
the immediate `Canvas` API.
