# rendy

A C++20 rendering library built on Vulkan 1.3 — fast 2D UI with real CSS
(flexbox, hot reload), forward-rendered PBR 3D with lights and shadows, glTF
model loading, text, and audio. For games, editors, and anything in between.

```cpp
#include <rendy/rendy.hpp>

int main() {
    auto app = rendy::App::create({.title = "hello", .size = {1280, 720}}).value();
    while (app.pollEvents()) {
        auto frame = app.beginFrame({.clear = rendy::colors::slate});
        frame.canvas().drawRect({{100, 100}, {200, 120}},
                                {.color = 0xE74C3CFF_rgba, .cornerRadius = 12});
        frame.canvas().drawText("hej världen", {120, 260}, {.size = 24});
        frame.present();
    }
}
```

## Features

- **2D**: batched immediate-mode canvas (rects, images, text) — the whole UI
  is typically a single draw call. Rounded corners, borders and anti-aliasing
  via SDF; clipping without batch breaks.
- **UI + CSS**: retained element tree styled with a real CSS subset —
  flexbox (Yoga), `width`/`height`, padding/margin, `:hover`, hot reload of
  `.css` files — plus an equivalent typed C++ styling API.
- **Text**: FreeType glyph atlas tuned for editor-crisp UI text.
- **3D**: forward PBR (metallic-roughness), directional/point/spot lights,
  cascaded + cube shadow maps, MSAA, HDR + tonemapping, all basic primitives,
  glTF 2.0 loading (fastgltf).
- **Audio**: WAV/OGG playback with a simple mixer (SDL3).
- **Performance first**: Vulkan 1.3 dynamic rendering, bindless textures,
  per-frame linear allocators, two frames in flight.

## Building

Requires: Linux with a Vulkan 1.3 driver, CMake ≥ 3.28, GCC 11+/Clang 14+.
All other dependencies are fetched and pinned automatically.

```sh
cmake --preset release
cmake --build --preset release
./build/release/examples/01_hello/01_hello
```

## Documentation

- [Getting started](docs/getting-started.md)
- Guides: [2D & Canvas](docs/guides/2d-canvas.md) · [UI & CSS](docs/guides/ui-css.md) ·
  [3D](docs/guides/3d.md) · [Audio](docs/guides/audio.md)
- [API reference](docs/api/)
- [Internals](docs/internals/) — architecture notes for contributors

## Examples

| | |
|---|---|
| `01_hello` | window, rounded box, text |
| `02_ui_gallery` | flexbox layouts, lists, hover states, live CSS reload |
| `03_text_editor_lite` | scrollable text file viewer with cursor |
| `04_scene3d` | primitives, moving lights, cascaded shadows |
| `05_model_viewer` | glTF viewer with orbit camera |
| `06_breakout` | mini-game: 3D + CSS HUD + sound |
| `07_audio` | mixer playground |

## License

MIT
