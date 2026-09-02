# 2D drawing with Canvas

`Canvas` is rendy's immediate-mode 2D API. Each frame you re-issue your draw
calls; the renderer batches every rect, image and glyph into one instanced
draw call, so thousands of items cost about the same as ten.

Get the canvas from the frame:

```cpp
auto frame = app.beginFrame({.clear = colors::slate});
auto canvas = frame.canvas();
```

Coordinates are pixels, origin top-left, +y down. `canvas.size()` is the
framebuffer size this frame.

## Rectangles

```cpp
canvas.drawRect({{x, y}, {w, h}}, {
    .color = 0x313244FF_rgba,
    .cornerRadius = 12.0f,        // or .cornerRadii = {tl, tr, br, bl}
    .borderWidth = 2.0f,
    .borderColor = colors::white,
});
```

- Corners, borders and anti-aliasing come from an analytic SDF in the
  fragment shader — no extra geometry, always smooth.
- A radius larger than half the box clamps CSS-style, so
  `.cornerRadius = 999` makes a pill/circle.
- Colors are sRGB with straight alpha; blending is done correctly in linear
  space. `Color::rgba(0xRRGGBBAA)`, `Color::rgb(0xRRGGBB)`, or the literals
  `0xE74C3CFF_rgba` / `0xE74C3C_rgb` (import `using namespace rendy;`).

## Images

```cpp
auto texture = app.loadTexture("logo.png").value();   // PNG/JPEG/BMP/TGA/HDR
canvas.drawImage(texture, {{50, 50}, {256, 256}}, {
    .tint = colors::white,
    .uv = {{0, 0}, {1, 1}},       // sub-rectangle for atlases/spritesheets
    .cornerRadius = 8.0f,
});
```

`TextureOptions` at load time controls filtering (`Linear`/`Nearest` — use
Nearest for pixel art), wrapping, sRGB and mipmaps. Textures are bindless:
drawing 100 different images still batches into one draw call.

## Text

```cpp
canvas.drawText("hej!", {x, y}, {.font = app.defaultFont(), .size = 16,
                                 .color = colors::white});
```

- `pos` is the **top-left** of the first line; `'\n'` breaks lines. Returns
  the drawn size.
- `measureText` returns the size without drawing; `textMetrics` gives
  ascent/descent/lineHeight for precise layout (see `03_text_editor_lite`).
- The default font is a system sans-serif found at startup; load specific
  fonts with `app.loadFont("font.ttf")` (e.g. a monospace for editors).
- Text is shaped with HarfBuzz: kerning, ligatures and complex scripts
  (Arabic joining, Indic reordering) work in any loaded font. Full bidi
  (UAX#9 via SheenBidi): mixed LTR/RTL lines reorder correctly, including
  RTL-base paragraphs with embedded Latin and numbers in RTL text; the
  base direction is auto-detected from the line's first strong character.
- Glyphs are rasterized by FreeType with light hinting into an atlas and
  positioned on integer pixels — tuned for crisp UI/editor text, not for
  large decorative headlines (scale a texture for that).

## Clipping

```cpp
canvas.pushClip({{0, 0}, {200, viewHeight}});
// ... draws are clipped to the rect (nested pushes intersect) ...
canvas.popClip();
```

Clipping happens in the shader per-instance, so it never splits the batch —
scroll views are free.

## Paint order & cost model

Draw order is submission order (painter's algorithm). The full frame's 2D
content becomes: one buffer upload + one `vkCmdDraw`. Things that are cheap:
many quads, many textures, many clips. Things that don't exist in v1:
rotated quads, custom 2D shaders, per-vertex gradients, drop shadows (fake
them with a blurred-corner rect: big radius + translucent color).
