// 03_text_editor_lite: a small but real text editor — typing, selection-free
// editing, scrolling, save. Proves editor-grade text rendering: thousands of
// crisp glyphs still land in a single draw call.
//
//   03_text_editor_lite [file]     (no file: opens untitled.txt)
//
// Keys: arrows/home/end/pgup/pgdn move, type to insert, enter/backspace/
// delete edit, ctrl+s saves, escape quits.

#include <rendy/rendy.hpp>

#include <algorithm>
#include <fstream>
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
    for (const char* fontPath :
         {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
          "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"}) {
        if (auto loaded = app.loadFont(fontPath)) {
            mono = loaded.value();
            break;
        }
    }

    const std::string path = argc > 1 ? argv[1] : "untitled.txt";
    std::vector<std::string> lines = loadLines(path);
    if (argc <= 1 && lines.size() == 1 && lines[0].empty())
        lines = {"// untitled.txt — skriv något!", ""};

    const float fontSize = 15.0f;
    float scrollY = 0.0f;
    int cursorLine = 0;
    int cursorCol = 0;
    bool modified = false;
    std::string statusMessage;
    double statusUntil = 0.0;

    auto currentLine = [&]() -> std::string& {
        return lines[static_cast<size_t>(cursorLine)];
    };
    auto clampCursor = [&] {
        cursorLine = std::clamp(cursorLine, 0, static_cast<int>(lines.size()) - 1);
        cursorCol = std::clamp(cursorCol, 0, static_cast<int>(currentLine().size()));
    };

    while (app.pollEvents()) {
        const Input& input = app.input();
        if (input.keyPressed(Key::Escape)) app.quit();

        // ---- editing ---------------------------------------------------
        if (!input.text().empty() && !input.ctrl()) {
            currentLine().insert(static_cast<size_t>(cursorCol), input.text());
            cursorCol += static_cast<int>(input.text().size());
            modified = true;
        }
        if (input.keyPressed(Key::Enter)) {
            std::string rest = currentLine().substr(static_cast<size_t>(cursorCol));
            currentLine().resize(static_cast<size_t>(cursorCol));
            lines.insert(lines.begin() + cursorLine + 1, std::move(rest));
            cursorLine++;
            cursorCol = 0;
            modified = true;
        }
        if (input.keyPressed(Key::Backspace)) {
            if (cursorCol > 0) {
                // Remove one UTF-8 codepoint backwards.
                auto& line = currentLine();
                int erase = 1;
                while (cursorCol - erase > 0 &&
                       (static_cast<unsigned char>(line[static_cast<size_t>(cursorCol - erase)]) & 0xC0) == 0x80)
                    erase++;
                line.erase(static_cast<size_t>(cursorCol - erase), static_cast<size_t>(erase));
                cursorCol -= erase;
                modified = true;
            } else if (cursorLine > 0) {
                cursorCol = static_cast<int>(lines[static_cast<size_t>(cursorLine - 1)].size());
                lines[static_cast<size_t>(cursorLine - 1)] += currentLine();
                lines.erase(lines.begin() + cursorLine);
                cursorLine--;
                modified = true;
            }
        }
        if (input.keyPressed(Key::Delete)) {
            auto& line = currentLine();
            if (cursorCol < static_cast<int>(line.size())) {
                int erase = 1;
                while (cursorCol + erase < static_cast<int>(line.size()) &&
                       (static_cast<unsigned char>(line[static_cast<size_t>(cursorCol + erase)]) & 0xC0) == 0x80)
                    erase++;
                line.erase(static_cast<size_t>(cursorCol), static_cast<size_t>(erase));
                modified = true;
            } else if (cursorLine + 1 < static_cast<int>(lines.size())) {
                line += lines[static_cast<size_t>(cursorLine + 1)];
                lines.erase(lines.begin() + cursorLine + 1);
                modified = true;
            }
        }
        if (input.ctrl() && input.keyPressed(Key::S)) {
            std::ofstream out(path, std::ios::trunc);
            for (size_t i = 0; i < lines.size(); ++i) out << lines[i] << '\n';
            statusMessage = out ? fmt::format("sparade {}", path)
                                : fmt::format("kunde inte spara {}", path);
            if (out) modified = false;
            statusUntil = app.time() + 2.5;
        }

        auto frame = app.beginFrame({.clear = 0x11111BFF_rgba});
        auto canvas = frame.canvas();
        const DrawTextOptions textOptions{.font = mono, .size = fontSize};
        const TextMetrics metrics = canvas.textMetrics(textOptions);
        const float lineHeight = metrics.lineHeight;
        const float viewHeight = canvas.size().y - 32.0f; // minus status bar
        const auto lineCount = static_cast<int>(lines.size());

        // ---- cursor movement -------------------------------------------
        scrollY -= input.wheel().y * lineHeight * 3.0f;
        const bool verticalMove = input.keyPressed(Key::Down) || input.keyPressed(Key::Up) ||
                                  input.keyPressed(Key::PageDown) || input.keyPressed(Key::PageUp);
        if (input.keyPressed(Key::Down)) cursorLine++;
        if (input.keyPressed(Key::Up)) cursorLine--;
        if (input.keyPressed(Key::PageDown)) cursorLine += static_cast<int>(viewHeight / lineHeight);
        if (input.keyPressed(Key::PageUp)) cursorLine -= static_cast<int>(viewHeight / lineHeight);
        if (input.keyPressed(Key::Right)) {
            if (cursorCol < static_cast<int>(currentLine().size())) {
                cursorCol++;
                while (cursorCol < static_cast<int>(currentLine().size()) &&
                       (static_cast<unsigned char>(currentLine()[static_cast<size_t>(cursorCol)]) & 0xC0) == 0x80)
                    cursorCol++;
            } else if (cursorLine + 1 < lineCount) {
                cursorLine++;
                cursorCol = 0;
            }
        }
        if (input.keyPressed(Key::Left)) {
            if (cursorCol > 0) {
                cursorCol--;
                while (cursorCol > 0 &&
                       (static_cast<unsigned char>(currentLine()[static_cast<size_t>(cursorCol)]) & 0xC0) == 0x80)
                    cursorCol--;
            } else if (cursorLine > 0) {
                cursorLine--;
                cursorCol = static_cast<int>(currentLine().size());
            }
        }
        if (input.keyPressed(Key::Home)) cursorCol = 0;
        if (input.keyPressed(Key::End)) cursorCol = 1 << 20;
        clampCursor();

        // Keep the cursor visible on vertical movement or edits.
        const float cursorY = static_cast<float>(cursorLine) * lineHeight;
        if (verticalMove || modified)
            scrollY = std::clamp(scrollY, cursorY - viewHeight + 2.0f * lineHeight, cursorY);
        scrollY = std::clamp(
            scrollY, 0.0f,
            std::max(0.0f, static_cast<float>(lineCount) * lineHeight - viewHeight * 0.5f));

        // ---- paint -----------------------------------------------------
        const float gutterWidth = 64.0f;
        const float textX = gutterWidth + 12.0f;
        canvas.drawRect({{0.0f, 0.0f}, {gutterWidth, viewHeight}}, {.color = 0x181825FF_rgba});

        const int firstLine = std::max(0, static_cast<int>(scrollY / lineHeight));
        const int lastLine =
            std::min(lineCount, firstLine + static_cast<int>(viewHeight / lineHeight) + 2);

        const float highlightY = cursorY - scrollY;
        canvas.drawRect({{gutterWidth, highlightY}, {canvas.size().x - gutterWidth, lineHeight}},
                        {.color = Color::rgba(0x31324460)});
        const float caretX =
            textX + canvas
                        .measureText(std::string_view(currentLine())
                                         .substr(0, static_cast<size_t>(cursorCol)),
                                     textOptions)
                        .x;
        // Blinking caret.
        if (std::fmod(app.time(), 1.0) < 0.6)
            canvas.drawRect({{caretX, highlightY + 2.0f}, {2.0f, lineHeight - 4.0f}},
                            {.color = 0xF5E0DCFF_rgba});

        canvas.pushClip({{0.0f, 0.0f}, {canvas.size().x, viewHeight}});
        for (int i = firstLine; i < lastLine; ++i) {
            const float y = static_cast<float>(i) * lineHeight - scrollY;
            canvas.drawText(fmt::format("{:>4}", i + 1), {12.0f, y},
                            {.font = mono, .size = fontSize,
                             .color = i == cursorLine ? Color::rgba(0xA6ADC8FF)
                                                      : Color::rgba(0x585B70FF)});
            canvas.drawText(lines[static_cast<size_t>(i)], {textX, y},
                            {.font = mono, .size = fontSize, .color = 0xCDD6F4FF_rgba});
        }
        canvas.popClip();

        // Status bar.
        canvas.drawRect({{0.0f, viewHeight}, {canvas.size().x, 32.0f}},
                        {.color = 0x181825FF_rgba});
        const std::string status =
            app.time() < statusUntil
                ? statusMessage
                : fmt::format("{}{}  —  {}:{}  —  ctrl+s sparar  —  {:.0f} fps", path,
                              modified ? " •" : "", cursorLine + 1, cursorCol + 1, app.fps());
        canvas.drawText(status, {12.0f, viewHeight + 7.0f},
                        {.size = 13.0f, .color = 0xA6ADC8FF_rgba});

        frame.present();
    }
    return 0;
}
