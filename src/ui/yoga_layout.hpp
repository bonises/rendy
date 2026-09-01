#pragma once

// ComputedStyle → Yoga node. Separate from the tree so layout mapping is
// unit-testable without a GPU or an App.

#include "css/computed.hpp"

#include <yoga/Yoga.h>

namespace rendy::ui {

void applyStyleToYoga(const css::ComputedStyle& style, YGNodeRef node);

} // namespace rendy::ui
