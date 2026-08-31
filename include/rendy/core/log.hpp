#pragma once

/// \file log.hpp
/// Leveled logging with fmt-style formatting:
///   rendy::log::info("loaded {} in {:.1f} ms", path, ms);
/// Default level is Info (Debug in debug builds). Output goes to stderr.

#include <fmt/core.h>

#include <string_view>

#include "result.hpp"

namespace rendy::log {

enum class Level { Trace, Debug, Info, Warn, Error, Off };

/// Set the minimum level that gets printed.
void setLevel(Level level);
Level level();

/// Core sink; the templated helpers below format and forward here.
void write(Level level, std::string_view message);

template <typename... Args>
void trace(fmt::format_string<Args...> f, Args&&... args) {
    if (level() <= Level::Trace) write(Level::Trace, fmt::format(f, std::forward<Args>(args)...));
}
template <typename... Args>
void debug(fmt::format_string<Args...> f, Args&&... args) {
    if (level() <= Level::Debug) write(Level::Debug, fmt::format(f, std::forward<Args>(args)...));
}
template <typename... Args>
void info(fmt::format_string<Args...> f, Args&&... args) {
    if (level() <= Level::Info) write(Level::Info, fmt::format(f, std::forward<Args>(args)...));
}
template <typename... Args>
void warn(fmt::format_string<Args...> f, Args&&... args) {
    if (level() <= Level::Warn) write(Level::Warn, fmt::format(f, std::forward<Args>(args)...));
}
template <typename... Args>
void error(fmt::format_string<Args...> f, Args&&... args) {
    if (level() <= Level::Error) write(Level::Error, fmt::format(f, std::forward<Args>(args)...));
}

} // namespace rendy::log

namespace rendy {

/// fmt-formatting overload of err(): `return rendy::err("bad size {}", n);`
template <typename... Args>
Error err(fmt::format_string<Args...> f, Args&&... args) {
    return Error{fmt::format(f, std::forward<Args>(args)...)};
}

} // namespace rendy
