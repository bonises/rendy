// 01_hello: a window, some rounded boxes, and (from M3) text.

#include <rendy/rendy.hpp>

#include <cmath>

using namespace rendy;

int main() {
    auto appResult = App::create({.title = "rendy — hello", .size = {1280, 720}});
    if (!appResult) {
        log::error("failed to start: {}", appResult.error().message);
        return 1;
    }
    auto app = std::move(appResult).value();

    while (app.pollEvents()) {
        if (app.input().keyPressed(Key::Escape)) app.quit();

        auto frame = app.beginFrame({.clear = colors::slate});
        auto canvas = frame.canvas();

        // A rounded card with a border.
        canvas.drawRect({{100.0f, 100.0f}, {320.0f, 180.0f}},
                        {.color = 0x313244FF_rgba,
                         .cornerRadius = 16.0f,
                         .borderWidth = 2.0f,
                         .borderColor = 0x585B70FF_rgba});

        // A pulsing accent dot.
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(app.time()) * 3.0f);
        const float r = 24.0f + 8.0f * pulse;
        canvas.drawRect({{160.0f - r, 190.0f - r}, {2.0f * r, 2.0f * r}},
                        {.color = 0xF38BA8FF_rgba, .cornerRadius = r});

        // Follow the mouse with a soft square.
        const Vec2 mouse = app.input().mousePos();
        canvas.drawRect({mouse - Vec2{20.0f}, {40.0f, 40.0f}},
                        {.color = Color::rgba(0x89B4FAB0), .cornerRadius = 12.0f});

        frame.present();
    }
    return 0;
}
