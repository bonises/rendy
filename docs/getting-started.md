# Getting started

## Requirements

- Linux with a Vulkan 1.3 capable GPU + driver (`libvulkan.so.1` installed).
  rendy requires dynamic rendering, synchronization2, timeline semaphores and
  descriptor indexing — standard on any recent desktop driver.
- CMake ≥ 3.28, GCC 11+ or Clang 14+.
- X11 development libraries (for SDL's window backend) and ALSA for audio —
  on Debian/Ubuntu: `libx11-dev libxext-dev libxrandr-dev libxcursor-dev
  libxi-dev libxkbcommon-dev libasound2-dev`.

Everything else (SDL3, FreeType, Yoga, glslang, fastgltf, …) is fetched and
pinned automatically by CMake on the first configure.

## Build

```sh
cmake --preset release          # or: debug (validation + shader hot reload), asan
cmake --build --preset release
ctest --preset release          # CPU-only unit tests, no GPU needed
```

Binaries land in `build/<preset>/examples/*/`.

## Your first window

```cpp
#include <rendy/rendy.hpp>
using namespace rendy;

int main() {
    auto app = App::create({.title = "min app", .size = {1280, 720}}).value();
    while (app.pollEvents()) {
        if (app.input().keyPressed(Key::Escape)) app.quit();

        auto frame = app.beginFrame({.clear = colors::slate});
        frame.canvas().drawRect({{100, 100}, {200, 120}},
                                {.color = 0xE74C3CFF_rgba, .cornerRadius = 12});
        frame.canvas().drawText("hej världen", {120, 260}, {.size = 24});
        frame.present();
    }
}
```

Link against `rendy::rendy`:

```cmake
add_subdirectory(path/to/rendy)
target_link_libraries(my_app PRIVATE rendy::rendy)
```

## The mental model

- **`App`** owns the window, GPU, input and frame pacing. You own the loop.
- **`Frame`** is one frame: get it from `beginFrame`, draw into it, `present()`
  (or just let it go out of scope).
- **`Canvas`** is immediate-mode 2D — call `drawRect`/`drawImage`/`drawText`
  every frame; everything batches into (typically) a single draw call.
- **`ui::Context`** is a retained element tree styled with CSS and flexbox,
  painted through the Canvas. Use it for application UI; use raw Canvas for
  game HUDs and custom widgets. They compose freely.
- **`Scene` + `Camera`** is retained 3D: add meshes/lights once, mutate
  transforms, then `frame.draw(scene, camera)` renders with PBR, shadows,
  MSAA and tonemapping. The 2D canvas always draws on top.
- **`audio::Mixer`** plays WAV/OGG/procedural sounds with volume/pan/loop.

Errors: anything that can fail returns `rendy::Result<T>` — check with
`if (result)`, take with `.value()`, read `.error().message`. rendy never
throws across its API.

## Where to next

- [2D & Canvas guide](guides/2d-canvas.md)
- [UI & CSS guide](guides/ui-css.md)
- [3D guide](guides/3d.md)
- [Audio guide](guides/audio.md)
- [API reference](api/)
- Examples in `examples/` — `01_hello` is the smallest, `06_breakout` shows
  everything composed.
