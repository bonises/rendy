# canvas

Headers: `rendy/canvas/{canvas,font}.hpp`

`Canvas` comes from `frame.canvas()`. Pixels, top-left origin. Submission
order = paint order. Everything batches into one instanced draw.

| Member | Signature |
|---|---|
| `drawRect` | `void drawRect(const Rect&, const DrawRectOptions& = {})` |
| `drawImage` | `void drawImage(TextureRef, const Rect&, const DrawImageOptions& = {})` |
| `drawText` | `Vec2 drawText(std::string_view, Vec2 topLeft, const DrawTextOptions& = {})` |
| `measureText` | `Vec2 measureText(std::string_view, const DrawTextOptions& = {})` |
| `drawTextWrapped` | `Vec2 drawTextWrapped(text, Vec2 topLeft, float maxWidth, options)` — greedy word wrap |
| `measureTextWrapped` | `Vec2 measureTextWrapped(text, float maxWidth, options)` |
| `textMetrics` | `TextMetrics textMetrics(const DrawTextOptions& = {})` |
| `pushClip` / `popClip` | intersecting clip stack, shader-side |
| `size` | `Vec2 size() const` |

**DrawRectOptions**: `color` (white), `cornerRadius` (0) or per-corner
`cornerRadii` {tl,tr,br,bl}, `borderWidth` (0), `borderColor` (black).
Radii clamp CSS-style (999 = pill).

**DrawImageOptions**: `tint` (white), `uv` sub-rect ({{0,0},{1,1}}),
`cornerRadius`.

**DrawTextOptions**: `font` (default = app default), `size` (16),
`color` (white).

**TextMetrics**: `ascent`, `descent`, `lineHeight` (px). `drawText`'s `pos`
is the top-left of the line box; the baseline sits at `pos.y + ascent`.
`'\n'` starts a new line. Returns drawn `{width, height}`.

**FontRef** (`font.hpp`): `{ uint32_t id; }` — id 0 is the app default.
