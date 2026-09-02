#pragma once

// Selector matching + cascade resolution. Elements are described through a
// small interface so the cascade is testable without the UI tree or a GPU.

#include "css/computed.hpp"
#include "css/stylesheet.hpp"

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace rendy::css {

/// What the matcher needs to know about an element.
struct MatchContext {
    std::string_view tag;
    std::string_view id;
    const std::vector<std::string>* classes = nullptr;
    uint8_t pseudo = 0; ///< PseudoFlags currently active
    /// Parent lookup for descendant/child combinators; nullptr at the root.
    const MatchContext* parent = nullptr;
};

[[nodiscard]] bool matchesCompound(const SimpleSelector& selector, const MatchContext& element);
[[nodiscard]] bool matchesSelector(const ComplexSelector& selector, const MatchContext& element);

/// One matched declaration source, ready for sorting.
struct MatchedRule {
    const Rule* rule = nullptr;
    uint32_t specificity = 0;
    uint32_t order = 0; ///< global source order (sheet index << 16 | rule index)
};

/// Collects rules from `sheets` (in load order) matching `element`, sorts by
/// (specificity, order), applies declarations onto `style` (which should be
/// initialized with defaults + inherited values), then applies `inlineStyle`
/// last (wins over everything).
void resolveStyle(const std::vector<const Stylesheet*>& sheets, const MatchContext& element,
                  const std::vector<ui::Declaration>* inlineStyle, ComputedStyle* style);

/// Applies one declaration to a computed style. em lengths resolve against
/// style->fontSize (font-size:em against the inherited size, so apply
/// font-size first — resolveStyle handles the ordering).
void applyDeclaration(const ui::Declaration& declaration, ComputedStyle* style);

} // namespace rendy::css
