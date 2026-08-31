#include "rendy/core/log.hpp"

#include <fmt/chrono.h>
#include <fmt/color.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace rendy::log {
namespace {

#ifdef NDEBUG
std::atomic<Level> g_level{Level::Info};
#else
std::atomic<Level> g_level{Level::Debug};
#endif
std::mutex g_writeMutex;

struct LevelStyle {
    const char* name;
    fmt::terminal_color color;
};

LevelStyle styleFor(Level level) {
    switch (level) {
    case Level::Trace: return {"TRACE", fmt::terminal_color::bright_black};
    case Level::Debug: return {"DEBUG", fmt::terminal_color::cyan};
    case Level::Info: return {"INFO ", fmt::terminal_color::green};
    case Level::Warn: return {"WARN ", fmt::terminal_color::yellow};
    case Level::Error: return {"ERROR", fmt::terminal_color::red};
    case Level::Off: break;
    }
    return {"?????", fmt::terminal_color::white};
}

} // namespace

void setLevel(Level level) { g_level.store(level, std::memory_order_relaxed); }
Level level() { return g_level.load(std::memory_order_relaxed); }

void write(Level level, std::string_view message) {
    const LevelStyle style = styleFor(level);
    const auto now = std::chrono::system_clock::now();
    const auto timeOfDay = std::chrono::floor<std::chrono::milliseconds>(now);

    std::lock_guard lock(g_writeMutex);
    fmt::print(stderr, "{:%H:%M:%S} ", timeOfDay);
    fmt::print(stderr, fmt::fg(style.color), "{}", style.name);
    fmt::print(stderr, " {}\n", message);
}

} // namespace rendy::log
