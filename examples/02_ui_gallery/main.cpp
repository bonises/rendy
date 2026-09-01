// 02_ui_gallery: flexbox layouts, a scrollable list, hover/active states,
// click handlers, and live hot reload of gallery.css.

#include <rendy/rendy.hpp>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace rendy;

namespace {

// Find gallery.css next to the executable or in the source tree.
std::string findCss(const char* argv0) {
    namespace fs = std::filesystem;
    const fs::path candidates[] = {
        fs::path(argv0).parent_path() / "gallery.css",
        "examples/02_ui_gallery/gallery.css",
        "gallery.css",
    };
    for (const auto& path : candidates)
        if (fs::exists(path)) return path.string();
    return "gallery.css";
}

} // namespace

int main(int, char** argv) {
    auto appResult = App::create({.title = "rendy — ui gallery", .size = {1000, 700}});
    if (!appResult) {
        log::error("failed to start: {}", appResult.error().message);
        return 1;
    }
    auto app = std::move(appResult).value();

    ui::Context context(app);
    if (auto loaded = context.loadStylesheet(findCss(argv[0])); !loaded)
        log::warn("{}", loaded.error().message);

    auto root = context.root();

    // Sidebar with single-selection nav.
    auto sidebar = root.addChild("div", {.classes = "sidebar"});
    sidebar.addChild("div", {.classes = "title", .text = "rendy gallery"});
    auto navItems = std::make_shared<std::vector<ui::Element>>();
    for (const char* page : {"Buttons", "Lists", "Layout", "Typography", "Settings"}) {
        auto item = sidebar.addChild("div", {.classes = "nav-item", .text = page});
        navItems->push_back(item);
        item.onClick([navItems](ui::Element clicked) {
            for (auto& other : *navItems) other.removeClass("selected");
            clicked.addClass("selected");
        });
    }
    navItems->front().addClass("selected");

    // Main column: toolbar, scrollable list, status bar.
    auto main = root.addChild("div", {.classes = "main"});
    auto toolbar = main.addChild("div", {.classes = "toolbar"});
    auto list = main.addChild("div", {.classes = "card"});
    auto status = main.addChild("div", {.classes = "statusbar"});
    auto fpsLabel = status.addChild("div", {.text = "fps"});
    status.addChild("div", {.text = "redigera gallery.css för hot reload"});

    auto clickCount = std::make_shared<int>(0);
    auto rowCount = std::make_shared<int>(0);
    auto counter = toolbar.addChild("div", {.classes = "counter", .text = "0 clicks"});

    const char* names[] = {"Alpha", "Beta", "Gamma", "Delta", "Epsilon", "Zeta",
                           "Eta",   "Theta", "Iota", "Kappa", "Lambda",  "My",
                           "Ny",    "Xi",   "Omikron", "Pi",  "Rho",     "Sigma"};
    auto addRow = [list, counter, clickCount, rowCount, &names]() mutable {
        const char* name = names[*rowCount % static_cast<int>(std::size(names))];
        auto row = list.addChild("div", {.classes = "row"});
        row.addChild("div", {.classes = "index", .text = fmt::format("#{:02}", ++*rowCount)});
        row.addChild("div", {.classes = "name", .text = name});
        row.addChild("div", {.classes = "badge", .text = "aktiv"});
        row.onClick([counter, clickCount](ui::Element) mutable {
            counter.setText(fmt::format("{} clicks", ++*clickCount));
        });
    };
    for (size_t i = 0; i < std::size(names); ++i) addRow();

    toolbar.addChild("button", {.classes = "primary", .text = "Add row"})
        .onClick([addRow](ui::Element) mutable { addRow(); });
    toolbar.addChild("button", {.text = "Clear"}).onClick([list, rowCount](ui::Element) mutable {
        list.clearChildren();
        *rowCount = 0;
    });
    toolbar.addChild("button", {.text = "Disabled"}).setDisabled(true);

    double lastFpsUpdate = 0.0;
    while (app.pollEvents()) {
        if (app.input().keyPressed(Key::Escape)) app.quit();
        if (std::getenv("RENDY_AUTOQUIT") != nullptr && app.time() > 2.5) app.quit();

        if (app.time() - lastFpsUpdate > 0.25) {
            lastFpsUpdate = app.time();
            fpsLabel.setText(fmt::format("{:.0f} fps — {} rader", app.fps(), *rowCount));
        }

        auto frame = app.beginFrame({});
        context.update();
        context.paint(frame.canvas());
        frame.present();
    }
    return 0;
}
