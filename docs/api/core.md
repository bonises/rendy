# core

Headers: `rendy/core/{result,log,color,rect,handle}.hpp`

## Result & errors — `result.hpp`

| Symbol | Signature | Notes |
|---|---|---|
| `Error` | `{ std::string message; }` | |
| `err` | `err(std::string)` / `err(fmt, args...)` | fmt overload lives in `log.hpp` |
| `Result<T>` | holds `T` or `Error` | `[[nodiscard]]` |
| | `bool hasValue()`, `explicit operator bool` | |
| | `T& value()` / `T&& value() &&` | asserts on error in debug |
| | `T valueOr(T fallback)` | |
| | `const Error& error()` | asserts on success in debug |
| `Result<void>` | default-constructed = success | |

## Logging — `log.hpp` (namespace `rendy::log`)

| Symbol | Notes |
|---|---|
| `trace/debug/info/warn/error(fmt, args...)` | fmt-style; to stderr with time + colored level |
| `setLevel(Level)`, `level()` | default `Info` (release) / `Debug` (debug builds) |

## Color — `color.hpp`

sRGB floats 0–1, straight alpha.

| Symbol | Notes |
|---|---|
| `Color{r,g,b,a}` | aggregate |
| `Color::rgba(0xRRGGBBAA)` / `Color::rgb(0xRRGGBB)` | constexpr |
| `color.fade(f)` | alpha × f |
| `color.packed()` | → `0xRRGGBBAA` |
| `0x..._rgba` / `0x..._rgb` literals | in `rendy::literals` (inline) |
| `colors::` | `transparent black white red green blue yellow orange purple gray slate` |

## Rect — `rect.hpp`

`{ Vec2 pos; Vec2 size; }`, top-left origin. `left/top/right/bottom/center`,
`empty`, `contains(Vec2)` (half-open), `intersect`, `overlaps`,
`expanded(amount)`.

## Handles — `handle.hpp`

| Symbol | Notes |
|---|---|
| `Handle<Tag>` | 32-bit index + generation; `valid()`, `==` |
| `HandlePool<T, Tag>` | `create(args...)`, `get(h)` → `T*` or nullptr (stale-safe), `destroy(h)`, `aliveCount()`, `forEach(f)` |
