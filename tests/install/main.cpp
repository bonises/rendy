// Exercises the installed package's public surface: window + 2D + text +
// a 3D primitive — enough to catch missing archive members or headers.

#include <rendy/rendy.hpp>

#include <cstdlib>

int main() {
    using namespace rendy;
    auto app = App::create({.title = "rendy consumer", .size = {640, 360}}).value();

    Scene scene(app);
    scene.addLight({.type = Light::Type::Directional, .direction = {-1.0f, -2.0f, -1.0f}});
    scene.addMesh(primitives::cube(), scene.defaultMaterial(), {});
    Camera camera;
    camera.lookAt({2.5f, 2.0f, 3.5f}, {0.0f, 0.0f, 0.0f});

    while (app.pollEvents()) {
        if (std::getenv("RENDY_AUTOQUIT") != nullptr && app.time() > 1.5) app.quit();
        auto frame = app.beginFrame({.clear = Color::rgb(0x224466)});
        frame.draw(scene, camera);
        frame.canvas().drawRect({{40, 40}, {180, 90}},
                                {.color = Color::rgb(0xE74C3C), .cornerRadius = 14});
        frame.canvas().drawText("installerad rendy", {50, 160},
                                {.font = app.defaultFont(), .size = 22});
        frame.present();
    }
    return 0;
}
