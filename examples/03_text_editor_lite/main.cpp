// 03_text_editor_lite: scrollable file viewer with a movable cursor.
// Proves editor-grade text: thousands of crisp glyphs, still one draw call.

#include <rendy/rendy.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace rendy;

namespace {

std::vector<std::string> loadLines(const std::string& path) {
    std::ifstream file(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        // Expand tabs so columns line up in a mono font.
        std::string expanded;
        for (char c : line) {
            if (c == '\t')
                expanded.append(4 - expanded.size() % 4, ' ');
            else
                expanded.push_back(c);
        }
        lines.push_back(std::move(expanded));
    }
    if (lines.empty()) lines.emplace_back();
    return lines;
}

} // namespace

int main(int argc, char** argv) {
    auto appResult = App::create({.title = "rendy — text editor", .size = {1100, 800}});
    if (!appResult) {
        log::error("failed to start: {}", appResult.error().message);
        return 1;
    }
    auto app = std::move(appResult).value();

    FontRef mono = app.defaultFont();
    for (const char* path : {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                             "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"}) {
        if (auto loaded = app.loadFont(path)) {
            mono = loaded.value();
            break;
        }
    }

    const std::string path = argc > 1 ? argv[1] : __FILE__;
    std::vector<std::string> lines = loadLines(path);

    const float fontSize = 15.0f;
    float scrollY = 0.0f;
    int cursorLine = 0;
    int cursorCol = 0;

    while (app.pollEvents()) {
        const Input& input = app.input();
        if (input.keyPressed(Key::Escape)) app.quit();

        auto frame = app.beginFrame({.clear = 0x11111BFF_rgba});
        auto canvas = frame.canvas();
        const DrawTextOptions textOptions{.font = mono, .size = fontSize};
        const TextMetrics metrics = canvas.textMetrics(textOptions);
        const float lineHeight = metrics.lineHeight;
        const float viewHeight = canvas.size().y;
        const auto lineCount = static_cast<int>(lines.size());

        // --- input ------------------------------------------------------
        scrollY -= input.wheel().y * lineHeight * 3.0f;
        if (input.keyPressed(Key::Down)) cursorLine++;
        if (input.keyPressed(Key::Up)) cursorLine--;
        if (input.keyPressed(Key::PageDown)) cursorLine += static_cast<int>(viewHeight / lineHeight);
        if (input.keyPressed(Key::PageUp)) cursorLine -= static_cast<int>(viewHeight / lineHeight);
        if (input.keyPressed(Key::Right)) cursorCol++;
        if (input.keyPressed(Key::Left)) cursorCol--;
        if (input.keyPressed(Key::Home)) cursorCol = 0;
        if (input.keyPressed(Key::End)) cursorCol = 1 << 20;
        cursorLine = std::clamp(cursorLine, 0, lineCount - 1);
        cursorCol = std::clamp(cursorCol, 0, static_cast<int>(lines[static_cast<size_t>(cursorLine)].size()));

        // Keep the cursor visible when moved with keys.
        const float cursorY = static_cast<float>(cursorLine) * lineHeight;
        if (input.keyPressed(Key::Down) || input.keyPressed(Key::Up) ||
            input.keyPressed(Key::PageDown) || input.keyPressed(Key::PageUp)) {
            scrollY = std::clamp(scrollY, cursorY - viewHeight + 2.0f * lineHeight, cursorY);
        }
        scrollY = std::clamp(scrollY, 0.0f,
                             std::max(0.0f, static_cast<float>(lineCount) * lineHeight - viewHeight * 0.5f));

        // --- paint ------------------------------------------------------
        const float gutterWidth = 64.0f;
        const float textX = gutterWidth + 12.0f;
        canvas.drawRect({{0.0f, 0.0f}, {gutterWidth, viewHeight}}, {.color = 0x181825FF_rgba});

        const int firstLine = std::max(0, static_cast<int>(scrollY / lineHeight));
        const int lastLine =
            std::min(lineCount, firstLine + static_cast<int>(viewHeight / lineHeight) + 2);

        // Cursor line highlight + caret.
        const float highlightY = cursorY - scrollY;
        canvas.drawRect({{gutterWidth, highlightY}, {canvas.size().x - gutterWidth, lineHeight}},
                        {.color = Color::rgba(0x31324460)});
        const std::string& cursorText = lines[static_cast<size_t>(cursorLine)];
        const float caretX =
            textX + canvas.measureText(std::string_view(cursorText).substr(0, static_cast<size_t>(cursorCol)),
                                       textOptions).x;
        canvas.drawRect({{caretX, highlightY + 2.0f}, {2.0f, lineHeight - 4.0f}},
                        {.color = 0xF5E0DCFF_rgba});

        for (int i = firstLine; i < lastLine; ++i) {
            const float y = static_cast<float>(i) * lineHeight - scrollY;
            canvas.drawText(fmt::format("{:>4}", i + 1), {12.0f, y},
                            {.font = mono, .size = fontSize,
                             .color = i == cursorLine ? Color::rgba(0xA6ADC8FF)
                                                      : Color::rgba(0x585B70FF)});
            canvas.drawText(lines[static_cast<size_t>(i)], {textX, y},
                            {.font = mono, .size = fontSize, .color = 0xCDD6F4FF_rgba});
        }

        canvas.drawText(fmt::format("{}  {}:{}  {:.0f} fps", path, cursorLine + 1,
                                    cursorCol + 1, app.fps()),
                        {12.0f, viewHeight - 24.0f}, {.size = 13.0f, .color = 0x6C7086FF_rgba});

        frame.present();
    }
    return 0;
}
