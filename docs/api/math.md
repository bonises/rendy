# math

Header: `rendy/math/math.hpp`. Thin aliases over GLM (with
`GLM_FORCE_DEPTH_ZERO_TO_ONE`), so every `glm::` function works on rendy
types.

| Symbol | Alias of |
|---|---|
| `Vec2 Vec3 Vec4` | `glm::vec2/3/4` |
| `IVec2 IVec3 UVec2` | `glm::ivec2/ivec3/uvec2` |
| `Mat3 Mat4` | `glm::mat3/mat4` |
| `Quat` | `glm::quat` (w, x, y, z constructor order) |
| `Pi TwoPi HalfPi` | constants |
| `radians(deg)` / `degrees(rad)` | constexpr |

## Transform

```cpp
struct Transform {
    Vec3 position{0};
    Quat rotation{1, 0, 0, 0};
    Vec3 scale{1};
    Mat4 matrix() const;   // T * R * S
};
```
