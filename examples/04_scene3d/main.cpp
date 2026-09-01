// 04_scene3d: every primitive on a plane, a sun, moving point/spot lights,
// PBR materials across a metallic/roughness spread, orbit camera, HUD.
// Shadows arrive in M6.

#include <rendy/rendy.hpp>

#include <cmath>
#include <cstdlib>
#include <string_view>

using namespace rendy;

int main(int argc, char** argv) {
    // 04_scene3d [instanceCount] [--no-vsync] — extra args spawn a field of
    // instanced cubes to stress the grouped-draw path.
    int stressCount = 0;
    bool vsync = true;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--no-vsync")
            vsync = false;
        else
            stressCount = std::atoi(argv[i]);
    }
    auto appResult = App::create(
        {.title = "rendy — scene3d", .size = {1440, 810}, .vsync = vsync});
    if (!appResult) {
        log::error("failed to start: {}", appResult.error().message);
        return 1;
    }
    auto app = std::move(appResult).value();

    Scene scene(app);
    scene.setAmbient(Color::rgb(0x1A1C22).fade(1.0f));

    // Ground.
    auto groundMaterial = scene.createMaterial({.baseColor = Color::rgb(0x8A8D93),
                                                .metallic = 0.0f,
                                                .roughness = 0.9f});
    scene.addMesh(primitives::plane({24.0f, 24.0f}), groundMaterial);

    // A row of primitives with varying metallic/roughness.
    struct Shape {
        MeshData mesh;
        Color color;
    };
    Shape shapes[] = {
        {primitives::cube(), Color::rgb(0xE74C3C)},
        {primitives::sphere(), Color::rgb(0x3498DB)},
        {primitives::cylinder(), Color::rgb(0x2ECC71)},
        {primitives::cone(), Color::rgb(0xF1C40F)},
        {primitives::capsule(), Color::rgb(0x9B59B6)},
        {primitives::torus(), Color::rgb(0xE67E22)},
    };
    NodeId spinners[std::size(shapes)];
    const float spacing = 1.8f;
    const float startX = -spacing * (static_cast<float>(std::size(shapes)) - 1.0f) * 0.5f;
    for (size_t i = 0; i < std::size(shapes); ++i) {
        const float t = static_cast<float>(i) / (std::size(shapes) - 1.0f);
        auto material = scene.createMaterial({.baseColor = shapes[i].color,
                                              .metallic = t,
                                              .roughness = 0.15f + 0.7f * (1.0f - t)});
        Transform transform;
        transform.position = {startX + spacing * static_cast<float>(i), 0.75f, 0.0f};
        spinners[i] = scene.addMesh(shapes[i].mesh, material, transform);
    }

    if (stressCount > 0) {
        // One mesh + one material shared by every instance → a handful of
        // instanced draw calls no matter the count.
        auto cubeMesh = scene.createMesh(primitives::cube({0.35f, 0.35f, 0.35f}));
        auto cubeMaterial = scene.createMaterial(
            {.baseColor = Color::rgb(0x94E2D5), .metallic = 0.3f, .roughness = 0.4f});
        const int side = std::max(1, static_cast<int>(std::sqrt(static_cast<float>(stressCount))));
        for (int i = 0; i < stressCount; ++i) {
            Transform t;
            t.position = {(i % side - side / 2) * 0.9f, 0.2f + 0.25f * ((i / side) % 3),
                          (i / side - side / 2) * 0.9f - 14.0f};
            scene.addMesh(cubeMesh, cubeMaterial, t);
        }
    }

    // A glass dome over the middle shapes (AlphaMode::Blend).
    auto glass = scene.createMaterial({.baseColor = Color::rgba(0x9AC8E840),
                                       .metallic = 0.0f,
                                       .roughness = 0.05f,
                                       .alphaMode = AlphaMode::Blend});
    scene.addMesh(primitives::sphere(2.2f, 48, 24), glass,
                  Transform{.position = {0.0f, 0.9f, 0.0f}});

    // Sun + moving lights.
    auto sun = scene.addLight({.type = Light::Type::Directional,
                               .direction = {-0.4f, -1.0f, -0.3f},
                               .color = Color::rgb(0xFFF4E0),
                               .intensity = 3.0f,
                               .castsShadows = true});
    auto pointA = scene.addLight({.type = Light::Type::Point,
                                  .color = Color::rgb(0x89B4FA),
                                  .intensity = 6.0f,
                                  .range = 8.0f});
    auto pointB = scene.addLight({.type = Light::Type::Point,
                                  .color = Color::rgb(0xF38BA8),
                                  .intensity = 6.0f,
                                  .range = 8.0f});
    auto spot = scene.addLight({.type = Light::Type::Spot,
                                .position = {0.0f, 0.0f, 0.0f},
                                .direction = {0.0f, -1.0f, 0.0f},
                                .color = Color::rgb(0xA6E3A1),
                                .intensity = 14.0f,
                                .range = 12.0f,
                                .innerCone = radians(18.0f),
                                .outerCone = radians(28.0f),
                                .castsShadows = true});
    scene.node(spot).setPosition({0.0f, 5.0f, 0.0f});
    (void)sun;

    Camera camera;
    float orbitAngle = 0.6f;
    float orbitHeight = 3.0f;
    float orbitDistance = 9.0f;

    while (app.pollEvents()) {
        const Input& input = app.input();
        if (input.keyPressed(Key::Escape)) app.quit();
        if (std::getenv("RENDY_AUTOQUIT") != nullptr && app.time() > 2.5) app.quit();

        // Orbit with arrows / drag, zoom with wheel.
        const float dt = app.dt();
        if (input.keyDown(Key::Left)) orbitAngle -= dt;
        if (input.keyDown(Key::Right)) orbitAngle += dt;
        if (input.mouseDown(MouseButton::Left)) {
            orbitAngle += input.mouseDelta().x * 0.005f;
            orbitHeight = std::clamp(orbitHeight + input.mouseDelta().y * 0.02f, 0.5f, 10.0f);
        }
        orbitDistance = std::clamp(orbitDistance - input.wheel().y * 0.8f, 3.0f, 24.0f);
        camera.lookAt({std::cos(orbitAngle) * orbitDistance, orbitHeight,
                       std::sin(orbitAngle) * orbitDistance},
                      {0.0f, 0.8f, 0.0f});

        // Animate.
        const auto time = static_cast<float>(app.time());
        for (size_t i = 0; i < std::size(spinners); ++i)
            scene.node(spinners[i]).rotateY(dt * (0.3f + 0.1f * static_cast<float>(i)));
        scene.node(pointA).setPosition(
            {std::cos(time * 0.7f) * 5.0f, 1.6f, std::sin(time * 0.7f) * 5.0f});
        scene.node(pointB).setPosition(
            {std::cos(time * 0.9f + Pi) * 4.0f, 1.2f, std::sin(time * 0.9f + Pi) * 4.0f});
        scene.node(spot).setPosition({std::sin(time * 0.5f) * 2.0f, 5.0f, 0.0f});

        auto frame = app.beginFrame({});
        frame.draw(scene, camera);
        frame.canvas().drawText(
            fmt::format("{:.0f} fps — dra med musen, scrolla för zoom", app.fps()),
            {12.0f, 10.0f}, {.size = 14.0f, .color = 0xCDD6F4CC_rgba});
        frame.present();
    }
    return 0;
}
