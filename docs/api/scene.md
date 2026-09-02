# scene

Headers: `rendy/scene/{scene,camera,light,material,mesh,primitives}.hpp` —
see the [3D guide](../guides/3d.md) for concepts.

## Scene

| Member | Signature | Notes |
|---|---|---|
| ctor | `Scene(App&)` | |
| `createMesh` | `MeshHandle createMesh(const MeshData&)` | uploads to GPU |
| `createMaterial` | `MaterialHandle createMaterial(const MaterialDesc&)` | |
| `defaultMaterial` | `MaterialHandle` (id 0) | neutral gray |
| `addMesh` | `NodeId addMesh(MeshData\|MeshHandle, MaterialHandle, Transform = {})` | |
| `addNode` | `NodeId addNode(Transform = {}, NodeId parent = {})` | empty/parent node |
| `addLight` | `NodeId addLight(const Light&, Transform = {})` | |
| `setParent / removeNode` | | removeNode kills the subtree |
| `node` | `NodeRef node(NodeId)` | chainable proxy |
| `transform` | `Transform& transform(NodeId)` | |
| `light` | `Light& light(NodeId)` | for addLight nodes |
| `setMaterial` | `void setMaterial(NodeId, MaterialHandle)` | |
| `setMorphWeights` | `void setMorphWeights(NodeId, std::vector<float>)` | shape-key weights |
| `setAmbient` | `void setAmbient(Color)` | flat ambient (no environment) |
| `setEnvironment` | `Result<void> setEnvironment(hdrPath, float intensity = 1)` | skybox + IBL from an equirect .hdr |
| `setEnvironmentIntensity` / `clearEnvironment` | | |
| `addReflectionProbe` | `ReflectionProbe addReflectionProbe(const ReflectionProbeDesc&)` | local parallax-corrected reflections (max 8) |
| `removeReflectionProbe` | `void removeReflectionProbe(ReflectionProbe)` | |
| `bakeReflectionProbes` | `void bakeReflectionProbes()` | blocking capture + GGX prefilter of every probe |
| `loadGltf` | `Result<NodeId> loadGltf(path)` | .gltf/.glb → root node (incl. skins + animations; Draco + KTX2/BasisU supported) |
| `animationNames` | `std::vector<std::string> animationNames() const` | loaded clips |
| `findAnimation` | `AnimationHandle findAnimation(std::string_view) const` | |
| `playAnimation` | `void playAnimation(handle\|name, bool loop = true, float speed = 1)` | |
| `stopAnimation` / `stopAllAnimations` / `animationPlaying` | | |
| `setAnimationWeight` | `void setAnimationWeight(handle, float)` | relative blend weight |
| `crossfadeAnimation` | `void crossfadeAnimation(handle\|name, float fadeSeconds, loop, speed)` | fade in, others fade out |
| `updateAnimations` | `void updateAnimations(float dt)` | once per frame |
| `approximateRadius` | `float approximateRadius(NodeId)` | subtree bounding radius |

`NodeRef`: `setPosition setRotation setScale rotateY rotate(angle, axis)
transform()` — all chainable.

## Camera

`{ position, rotation, fovY (60°), nearPlane (0.1), farPlane (300) }` with
`lookAt(eye, target, up = +y)`, `forward/right/up()`, `view()`,
`proj(aspect)`.

## Light

`{ type (Directional/Point/Spot), position, direction, color, intensity,
range (0 = unbounded), innerCone, outerCone, castsShadows }` — position and
direction are relative to the light's node transform.

## MaterialDesc

`{ baseColor, metallic (0), roughness (0.7), emissive, normalScale,
occlusionStrength, alphaMode (Opaque/Mask/Blend), alphaCutoff (0.5),
baseColorTexture, metallicRoughnessTexture (G=roughness
B=metallic, linear), normalTexture (linear), occlusionTexture (linear),
emissiveTexture }`. Color factors are sRGB and multiply the textures.

## MeshData & Vertex

```cpp
struct Vertex { Vec3 position; Vec3 normal; Vec4 tangent; Vec2 uv; };
struct MeshData { std::vector<Vertex> vertices; std::vector<uint32_t> indices;
                  Vec3 boundsCenter; float boundsRadius; };  // radius 0 = computed
```

CCW winding seen from outside; back faces culled.

## primitives::

`cube(size)`, `sphere(radius, segments, rings)`, `plane(size)` (XZ, +y
normal), `cylinder(radius, height, segments)`, `cone(...)`,
`capsule(radius, cylinderHeight, segments, rings)`,
`torus(majorRadius, minorRadius, segments, sides)` — all return `MeshData`
centered at origin with normals/tangents/UVs.
