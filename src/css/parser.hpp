#pragma once

// CSS parser: tokens → Stylesheet. Unknown properties/selectors are skipped
// (collected in Stylesheet::unsupported), never fatal — a stylesheet parses
// as long as braces balance.

#include "css/stylesheet.hpp"
#include "rendy/core/result.hpp"

#include <string_view>

namespace rendy::css {

Result<Stylesheet> parse(std::string_view cssText);

/// Parses a CSS color ("#aabbcc", "rgb(1,2,3)", "red"...). Exposed for tests.
bool parseColorText(std::string_view text, Color* out);

} // namespace rendy::css
