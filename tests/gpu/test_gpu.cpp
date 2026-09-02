// GPU smoke tests: real device, hidden window, screenshot readback.
// Robust invariants (pixel colors, image diffs) — no brittle golden hashes.
// Opt-in via RENDY_GPU_TESTS; each case owns its App (SDL init/quit cycles).

#include <catch2/catch_test_macros.hpp>

#include <rendy/rendy.hpp>

#include <cmath>
#include <cstdlib>

using namespace rendy;

namespace {

constexpr IVec2 kSize{640, 360};

App makeApp() {
    auto app = App::create({.title = "gpu-test", .size = kSize, .vsync = false,
                            .hidden = true});
    if (!app) // surface the actual error: headless vs driver failures differ
        FAIL("App::create failed: " << app.error().message);
    return std::move(app.value());
}

/// Runs one frame with `draw` and returns its pixels.
template <typename Draw>
Screenshot renderOnce(App& app, Color clear, Draw&& draw) {
    app.pollEvents();
    app.requestScreenshot();
    {
        auto frame = app.beginFrame({.clear = clear});
        draw(frame);
        frame.present();
    }
    auto shot = app.takeScreenshot();
    REQUIRE(shot.hasValue());
    REQUIRE(shot.value().size.x > 0);
    return std::move(shot.value());
}

struct Pixel {
    int r, g, b, a;
};

Pixel at(const Screenshot& shot, int x, int y) {
    const size_t i =
        (static_cast<size_t>(y) * static_cast<size_t>(shot.size.x) + static_cast<size_t>(x)) *
        4;
    return {shot.rgba[i], shot.rgba[i + 1], shot.rgba[i + 2], shot.rgba[i + 3]};
}

bool closeTo(Pixel p, int r, int g, int b, int tolerance = 6) {
    return std::abs(p.r - r) <= tolerance && std::abs(p.g - g) <= tolerance &&
           std::abs(p.b - b) <= tolerance;
}

/// Mean absolute per-channel difference over the whole image.
double meanDiff(const Screenshot& a, const Screenshot& b) {
    REQUIRE(a.rgba.size() == b.rgba.size());
    double total = 0.0;
    for (size_t i = 0; i < a.rgba.size(); ++i)
        total += std::abs(static_cast<int>(a.rgba[i]) - static_cast<int>(b.rgba[i]));
    return total / static_cast<double>(a.rgba.size());
}

} // namespace

TEST_CASE("2d readback: clear color, solid rect, rounded corners", "[gpu]") {
    App app = makeApp();
    const Screenshot shot = renderOnce(app, Color::rgb(0x224466), [](Frame& frame) {
        frame.canvas().drawRect({{100, 100}, {200, 150}},
                                {.color = Color::rgb(0xFF0000), .cornerRadius = 40});
    });
    REQUIRE(shot.size == kSize);
    REQUIRE(closeTo(at(shot, 10, 10), 0x22, 0x44, 0x66));    // background
    REQUIRE(closeTo(at(shot, 200, 175), 0xFF, 0x00, 0x00));  // rect center
    REQUIRE(closeTo(at(shot, 103, 103), 0x22, 0x44, 0x66));  // cut by the corner radius
    REQUIRE(closeTo(at(shot, 200, 105), 0xFF, 0x00, 0x00));  // straight top edge

    // Text produces non-background pixels where drawn.
    const Screenshot text = renderOnce(app, colors::black, [&](Frame& frame) {
        frame.canvas().drawText("gpu", {50, 50},
                                {.font = app.defaultFont(), .size = 48,
                                 .color = colors::white});
    });
    int lit = 0;
    for (int y = 50; y < 110; ++y)
        for (int x = 50; x < 150; ++x)
            if (at(text, x, y).r > 128) lit++;
    REQUIRE(lit > 100);
}

TEST_CASE("screenshot api: take without request fails", "[gpu]") {
    App app = makeApp();
    REQUIRE_FALSE(app.takeScreenshot().hasValue());
    (void)renderOnce(app, colors::black, [](Frame&) {}); // request+present works
    REQUIRE_FALSE(app.takeScreenshot().hasValue());      // consumed by renderOnce
}

TEST_CASE("ui damage cache: replay is pixel-identical, edits invalidate", "[gpu]") {
    App app = makeApp();
    ui::Context ui(app);
    ui.addStylesheet("div { background-color: #313244; padding: 20px; }"
                     ".label { color: #f9e2af; font-size: 24px; }");
    auto label = ui.root().addChild("div").addChild("div", {.classes = "label",
                                                            .text = "damage"});

    const auto uiFrame = [&] {
        return renderOnce(app, colors::black, [&](Frame& frame) {
            ui.update();
            ui.paint(frame.canvas());
        });
    };
    const Screenshot recorded = uiFrame(); // first paint records
    const Screenshot replayed = uiFrame(); // nothing changed: replay path
    REQUIRE(meanDiff(recorded, replayed) == 0.0); // byte-identical

    label.setText("changed!"); // marks dirty → re-record
    const Screenshot edited = uiFrame();
    REQUIRE(meanDiff(recorded, edited) > 0.05);
}

TEST_CASE("3d renders a lit cube", "[gpu]") {
    App app = makeApp();
    Scene scene(app);
    scene.addLight({.type = Light::Type::Directional, .direction = {-1.0f, -2.0f, -1.0f},
                    .intensity = 3.0f});
    auto material = scene.createMaterial({.baseColor = Color::rgb(0xE67E22)});
    scene.addMesh(primitives::cube(), material, {});
    Camera camera;
    camera.lookAt({2.5f, 2.0f, 3.5f}, {0.0f, 0.0f, 0.0f});

    const Screenshot shot = renderOnce(app, colors::black, [&](Frame& frame) {
        frame.draw(scene, camera);
    });
    const Pixel center = at(shot, kSize.x / 2, kSize.y / 2);
    REQUIRE(center.r > 40);            // lit orange cube face
    REQUIRE(center.r > center.b + 20); // orange, not gray
    REQUIRE(closeTo(at(shot, 5, 5), 0, 0, 0, 10)); // background stays black
}

TEST_CASE("reflection probes: the last-baked scene owns the array", "[gpu]") {
    App app = makeApp();

    // Scene A: mirror sphere in a red-walled alcove + probe.
    Scene sceneA(app);
    sceneA.setAmbient(Color{0.08f, 0.08f, 0.08f, 1.0f});
    sceneA.addLight({.type = Light::Type::Directional, .direction = {-0.5f, -1.0f, -0.3f},
                     .intensity = 3.0f});
    auto red = sceneA.createMaterial({.baseColor = Color::rgb(0xC0392B), .roughness = 0.9f});
    sceneA.addMesh(primitives::cube({4.0f, 4.0f, 0.3f}), red,
                   Transform{.position = {0.0f, 0.0f, -2.0f}});
    sceneA.addMesh(primitives::cube({0.3f, 4.0f, 4.0f}), red,
                   Transform{.position = {-2.0f, 0.0f, 0.0f}});
    auto mirror = sceneA.createMaterial(
        {.baseColor = Color::rgb(0xF5F5F5), .metallic = 1.0f, .roughness = 0.05f});
    sceneA.addMesh(primitives::sphere(0.8f), mirror, {});
    sceneA.addReflectionProbe({.position = {0.0f, 0.0f, 0.0f},
                               .boxMin = {-2.0f, -2.0f, -2.0f},
                               .boxMax = {2.0f, 2.0f, 2.0f}});
    Camera camera;
    camera.lookAt({1.6f, 0.6f, 2.0f}, {0.0f, 0.0f, 0.0f});
    const auto renderA = [&] {
        return renderOnce(app, colors::black,
                          [&](Frame& frame) { frame.draw(sceneA, camera); });
    };

    const Screenshot unbaked = renderA(); // probe added but never baked: inert
    sceneA.bakeReflectionProbes();
    const Screenshot baked = renderA(); // sphere reflects the red walls
    REQUIRE(meanDiff(unbaked, baked) > 0.2); // the probe visibly contributes

    // Scene B bakes → the app-global probe array now belongs to B, so A's
    // stale `baked` flags must NOT sample B's capture: A renders exactly
    // as probe-less again (not with B's green reflections).
    Scene sceneB(app);
    sceneB.setAmbient(Color{0.9f, 0.9f, 0.9f, 1.0f}); // bright: B's capture is
    sceneB.addLight({.type = Light::Type::Directional, // unmistakably green
                     .direction = {0.0f, -1.0f, -0.2f}, .intensity = 5.0f});
    auto green = sceneB.createMaterial({.baseColor = Color::rgb(0x2ECC71)});
    for (const Vec3 offset : {Vec3{0.0f, 0.0f, -2.0f}, Vec3{0.0f, 0.0f, 2.0f},
                              Vec3{-2.0f, 0.0f, 0.0f}, Vec3{2.0f, 0.0f, 0.0f}})
        sceneB.addMesh(primitives::cube({4.0f, 4.0f, 0.3f}), green,
                       Transform{.position = offset,
                                 .rotation = offset.x != 0.0f
                                     ? glm::angleAxis(glm::radians(90.0f), Vec3{0, 1, 0})
                                     : Quat{1, 0, 0, 0}});
    sceneB.addReflectionProbe({.position = {0.0f, 0.0f, 0.0f},
                               .boxMin = {-2.0f, -2.0f, -2.0f},
                               .boxMax = {2.0f, 2.0f, 2.0f}});
    sceneB.bakeReflectionProbes();

    const Screenshot stolen = renderA();
    REQUIRE(meanDiff(stolen, unbaked) < 0.05); // probe-less, no foreign capture

    // Re-baking A restores its capture (deterministic → near-identical).
    sceneA.bakeReflectionProbes();
    const Screenshot rebaked = renderA();
    REQUIRE(meanDiff(baked, rebaked) < 0.05);
}
