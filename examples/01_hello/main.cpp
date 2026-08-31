// 01_hello: a window with a clear color. Grows a rounded box + text in M2/M3.

#include <rendy/rendy.hpp>

int main() {
    auto appResult = rendy::App::create({.title = "rendy — hello", .size = {1280, 720}});
    if (!appResult) {
        rendy::log::error("failed to start: {}", appResult.error().message);
        return 1;
    }
    auto app = std::move(appResult).value();

    while (app.pollEvents()) {
        if (app.input().keyPressed(rendy::Key::Escape)) app.quit();

        auto frame = app.beginFrame({.clear = rendy::colors::slate});
        frame.present();
    }
    return 0;
}
