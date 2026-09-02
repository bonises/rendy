# app

Headers: `rendy/app/{app,input}.hpp`

## App

| Member | Signature | Notes |
|---|---|---|
| `create` | `static Result<App> create(const AppConfig& = {})` | opens window + GPU |
| `pollEvents` | `bool pollEvents()` | pump events; false on quit |
| `quit` | `void quit()` | next pollEvents returns false |
| `beginFrame` | `Frame beginFrame(const FrameConfig& = {})` | |
| `loadTexture` | `Result<TextureRef> loadTexture(path, const TextureOptions& = {})` | PNG/JPEG/BMP/TGA/HDR |
| `createTexture` | `Result<TextureRef> createTexture(rgbaPixels, IVec2 size, options)` | tightly packed RGBA8 |
| `destroyTexture` | `void destroyTexture(TextureRef)` | safe while in flight |
| `loadFont` | `Result<FontRef> loadFont(path)` | .ttf/.otf |
| `defaultFont` | `FontRef defaultFont() const` | system sans (id 0) |
| `input` | `const Input& input() const` | |
| `pixelSize` | `IVec2 pixelSize() const` | framebuffer px |
| `dt` / `time` / `fps` | `float` / `double` / `float` | dt clamped to 0.1 s |
| `requestScreenshot` | `void requestScreenshot()` | captures the NEXT presented frame (that present blocks briefly) |
| `takeScreenshot` | `Result<Screenshot> takeScreenshot()` | `{IVec2 size, vector<uint8_t> rgba}` — sRGB, top-left origin |

`AppConfig`: `title` ("rendy"), `size` ({1280,720}), `resizable` (true),
`vsync` (true; off = mailbox/immediate), `validation` (false), `hidden`
(false; offscreen tests/tools). `FrameConfig`: `clear` color.

## Frame

Move-only; presents on destruction or explicit `present()`.

| Member | Notes |
|---|---|
| `canvas()` | 2D drawing surface (drawn last, on top) |
| `draw(Scene&, const Camera&)` | render one 3D scene under the 2D |
| `present()` | submit + present; idempotent |

## Input

Refreshed by `pollEvents`. "Pressed"/"released" are this-frame edges;
`keyPressed` repeats with OS key-repeat (editor-friendly).

| Member | Notes |
|---|---|
| `mousePos() mouseDelta() wheel()` | px / px / lines (+y up) |
| `mouseDown/Pressed/Released(MouseButton)` | Left, Right, Middle |
| `keyDown/Pressed/Released(Key)` | `Key::A..Z Num0-9 F1-12 Escape Enter Tab Backspace Space Insert Delete Home End PageUp/Down arrows modifiers punctuation` |
| `ctrl() shift() alt()` | either side |
| `text()` | UTF-8 typed this frame (layout/IME aware) |

## Textures — `rendy/gpu/texture.hpp`

`TextureRef { uint32_t index; IVec2 size; }` — bindless index, stable for
the texture's lifetime; `index 0` = built-in white.
`TextureOptions { filter (Linear/Nearest), wrap (Clamp/Repeat), srgb (true),
mipmaps (false) }`.
