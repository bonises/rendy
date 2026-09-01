// 05_model_viewer: load a .gltf/.glb, orbit around it, tweak the light.
//
//   05_model_viewer path/to/model.glb

#include <rendy/rendy.hpp>

#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

#include <fmt/ranges.h>

using namespace rendy;

int main(int argc, char** argv) {
    bool vsync = true;
    const char* modelPath = nullptr;
    const char* envPath = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--no-vsync")
            vsync = false;
        else if (arg.size() > 4 && arg.substr(arg.size() - 4) == ".hdr")
            envPath = argv[i];
        else
            modelPath = argv[i];
    }
    auto appResult =
        App::create({.title = "rendy — model viewer", .size = {1440, 810}, .vsync = vsync});
    if (!appResult) {
        log::error("failed to start: {}", appResult.error().message);
        return 1;
    }
    auto app = std::move(appResult).value();

    Scene scene(app);
    scene.setAmbient(Color::rgb(0x202430));
    if (envPath != nullptr) {
        if (auto set = scene.setEnvironment(envPath); !set)
            log::warn("{}", set.error().message);
    }

    auto ground = scene.createMaterial({.baseColor = Color::rgb(0x3A3D45), .roughness = 0.85f});
    scene.addMesh(primitives::plane({30.0f, 30.0f}), ground,
                  Transform{.position = {0.0f, -1.0f, 0.0f}});

    scene.addLight({.type = Light::Type::Directional,
                    .direction = {-0.5f, -1.0f, -0.4f},
                    .color = Color::rgb(0xFFF2DC),
                    .intensity = 3.5f,
                    .castsShadows = true});
    auto fillLight = scene.addLight({.type = Light::Type::Point,
                                     .position = {0.0f, 2.0f, 0.0f},
                                     .color = Color::rgb(0xBAC8FF),
                                     .intensity = 4.0f,
                                     .range = 12.0f});

    NodeId model{};
    const char* path = modelPath != nullptr ? modelPath : "DamagedHelmet.glb";
    if (auto loaded = scene.loadGltf(path)) {
        model = loaded.value();
    } else {
        log::error("{}", loaded.error().message);
        log::info("kör med en modellväg: 05_model_viewer path/to/model.glb");
    }

    // Animations: play the first clip, cycle with A.
    const auto animationNames = scene.animationNames();
    int currentAnimation = -1;
    if (!animationNames.empty()) {
        currentAnimation = 0;
        scene.playAnimation(Scene::AnimationHandle{0});
        log::info("animations: {}", fmt::join(animationNames, ", "));
    }

    // Frame the camera around the model regardless of its unit scale.
    const float modelRadius = model.valid() ? scene.approximateRadius(model) : 2.0f;
    Camera camera;
    camera.farPlane = std::max(300.0f, modelRadius * 20.0f);
    float orbitAngle = 0.7f;
    float orbitPitch = 0.12f;
    float distance = modelRadius * 2.2f;
    const float targetY = modelRadius * 0.45f;
    bool spin = true;

    while (app.pollEvents()) {
        const Input& input = app.input();
        if (input.keyPressed(Key::Escape)) app.quit();
        if (input.keyPressed(Key::Space)) spin = !spin;
        if (input.keyPressed(Key::A) && !animationNames.empty()) {
            scene.stopAllAnimations();
            currentAnimation = (currentAnimation + 1) % static_cast<int>(animationNames.size());
            scene.playAnimation(
                Scene::AnimationHandle{static_cast<uint32_t>(currentAnimation)});
        }
        if (std::getenv("RENDY_AUTOQUIT") != nullptr && app.time() > 2.5) app.quit();

        if (input.mouseDown(MouseButton::Left)) {
            orbitAngle += input.mouseDelta().x * 0.006f;
            orbitPitch = std::clamp(orbitPitch + input.mouseDelta().y * 0.004f, -1.2f, 1.4f);
        }
        distance = std::clamp(distance - input.wheel().y * modelRadius * 0.2f,
                              modelRadius * 0.3f, modelRadius * 10.0f);

        if (model.valid() && spin) scene.node(model).rotateY(app.dt() * 0.4f);
        scene.updateAnimations(app.dt());
        scene.node(fillLight)
            .setPosition({std::cos(static_cast<float>(app.time())) * 3.0f, 2.5f,
                          std::sin(static_cast<float>(app.time())) * 3.0f});

        const Vec3 eye{std::cos(orbitAngle) * std::cos(orbitPitch) * distance,
                       std::sin(orbitPitch) * distance + targetY,
                       std::sin(orbitAngle) * std::cos(orbitPitch) * distance};
        camera.lookAt(eye, {0.0f, targetY, 0.0f});

        auto frame = app.beginFrame({});
        frame.draw(scene, camera);
        std::string status =
            fmt::format("{:.0f} fps — dra: rotera, scroll: zoom, space: {}", app.fps(),
                        spin ? "pausa" : "snurra");
        if (currentAnimation >= 0)
            status += fmt::format("  |  A: byt klipp — spelar \"{}\"",
                                  animationNames[static_cast<size_t>(currentAnimation)]);
        frame.canvas().drawText(status, {12.0f, 10.0f},
                                {.size = 14.0f, .color = 0xCDD6F4CC_rgba});
        frame.present();
    }
    return 0;
}
