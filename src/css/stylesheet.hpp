#pragma once

// Parsed CSS model: selectors, rules, stylesheets.

#include "rendy/ui/style.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace rendy::css {

enum PseudoFlags : uint8_t {
    kPseudoHover = 1 << 0,
    kPseudoActive = 1 << 1,
    kPseudoFocus = 1 << 2,
    kPseudoDisabled = 1 << 3,
    kPseudoFirstChild = 1 << 4,
    kPseudoLastChild = 1 << 5,
};

/// One compound selector: `button.primary:hover`.
struct SimpleSelector {
    std::string tag; ///< empty = any ("*" or omitted)
    std::string id;
    std::vector<std::string> classes;
    uint8_t pseudo = 0;
};

enum class Combinator : uint8_t { Descendant, Child };

/// `div > .list button:hover` — compounds right-to-left order is stored
/// left-to-right here; combinators[i] joins compounds[i] and compounds[i+1].
struct ComplexSelector {
    std::vector<SimpleSelector> compounds;
    std::vector<Combinator> combinators;

    /// (id, class+pseudo, type) packed for comparison.
    [[nodiscard]] uint32_t specificity() const {
        uint32_t ids = 0;
        uint32_t classes = 0;
        uint32_t types = 0;
        for (const SimpleSelector& simple : compounds) {
            if (!simple.id.empty()) ids++;
            classes += static_cast<uint32_t>(simple.classes.size());
            classes += static_cast<uint32_t>(__builtin_popcount(simple.pseudo));
            if (!simple.tag.empty()) types++;
        }
        return (ids << 20) | (classes << 10) | types;
    }
};

struct Rule {
    std::vector<ComplexSelector> selectors;
    std::vector<ui::Declaration> declarations;
};

/// One `@keyframes` step: `0%, 50% { ... }` becomes one Keyframe per offset
/// (offsets are fractions, `from`/`to` = 0/1). Frames with equal offsets are
/// merged during parsing (later declarations win).
struct Keyframe {
    float offset = 0.0f;
    std::vector<ui::Declaration> declarations;
};

struct KeyframesRule {
    std::string name;
    std::vector<Keyframe> frames; ///< sorted by offset
};

struct Stylesheet {
    std::vector<Rule> rules;
    std::vector<KeyframesRule> keyframes;
    /// Property names that parsed but aren't supported (for warnings).
    std::vector<std::string> unsupported;
};

} // namespace rendy::css
