#include "css/cascade.hpp"

#include <algorithm>

namespace rendy::css {

using ui::Declaration;
using ui::Length;
using ui::Prop;
using ui::Unit;

bool matchesCompound(const SimpleSelector& selector, const MatchContext& element) {
    if (!selector.tag.empty() && selector.tag != element.tag) return false;
    if (!selector.id.empty() && selector.id != element.id) return false;
    if ((selector.pseudo & element.pseudo) != selector.pseudo) return false;
    for (const std::string& cls : selector.classes) {
        if (element.classes == nullptr) return false;
        if (std::find(element.classes->begin(), element.classes->end(), cls) ==
            element.classes->end())
            return false;
    }
    return true;
}

namespace {

// Match compounds[0..index] where compounds[index] must be satisfied at
// `candidate` or (for descendant combinators) some ancestor of it. Needs
// backtracking: with mixed child/descendant combinators (".a > .b .c") the
// nearest matching ancestor is not always the right one.
bool matchAncestors(const ComplexSelector& selector, size_t index,
                    const MatchContext* candidate) {
    const SimpleSelector& compound = selector.compounds[index];
    const Combinator combinator = selector.combinators[index]; // joins to index+1

    for (const MatchContext* cursor = candidate; cursor != nullptr;
         cursor = cursor->parent) {
        if (matchesCompound(compound, *cursor)) {
            if (index == 0) return true;
            if (matchAncestors(selector, index - 1, cursor->parent)) return true;
            // else: try a higher ancestor (descendant only, see below).
        }
        if (combinator == Combinator::Child) return false; // exactly one try
    }
    return false;
}

} // namespace

bool matchesSelector(const ComplexSelector& selector, const MatchContext& element) {
    // Match right-to-left: rightmost compound against the element itself.
    const auto count = selector.compounds.size();
    if (count == 0) return false;
    if (!matchesCompound(selector.compounds[count - 1], element)) return false;
    if (count == 1) return true;
    return matchAncestors(selector, count - 2, element.parent);
}

void applyDeclaration(const Declaration& d, ComputedStyle* s) {
    using ui::Align;
    using ui::Display;
    using ui::FlexDirection;
    using ui::FlexWrap;
    using ui::Justify;
    using ui::Overflow;
    using ui::Position;
    using ui::TextAlign;

    auto resolveEm = [&](Length len) -> Length {
        if (len.unit == Unit::Em) return Length::px(len.value * s->fontSize);
        return len;
    };

    switch (d.prop) {
    // clang-format off
    case Prop::Display: s->display = static_cast<Display>(d.value.keyword); break;
    case Prop::FlexDirection: s->flexDirection = static_cast<FlexDirection>(d.value.keyword); break;
    case Prop::FlexWrap: s->flexWrap = static_cast<FlexWrap>(d.value.keyword); break;
    case Prop::JustifyContent: s->justifyContent = static_cast<Justify>(d.value.keyword); break;
    case Prop::AlignItems: s->alignItems = static_cast<Align>(d.value.keyword); break;
    case Prop::AlignSelf: s->alignSelf = static_cast<Align>(d.value.keyword); break;
    case Prop::AlignContent: s->alignContent = static_cast<Align>(d.value.keyword); break;
    case Prop::PositionType: s->position = static_cast<Position>(d.value.keyword); break;
    case Prop::Overflow: s->overflow = static_cast<Overflow>(d.value.keyword); break;
    case Prop::TextAlignProp: s->textAlign = static_cast<TextAlign>(d.value.keyword); break;
    case Prop::FlexGrow: s->flexGrow = d.value.number; break;
    case Prop::FlexShrink: s->flexShrink = d.value.number; break;
    case Prop::FlexBasis: s->flexBasis = resolveEm(d.value.length); break;
    case Prop::RowGap: s->rowGap = d.value.number; break;
    case Prop::ColumnGap: s->columnGap = d.value.number; break;
    case Prop::Width: s->width = resolveEm(d.value.length); break;
    case Prop::Height: s->height = resolveEm(d.value.length); break;
    case Prop::MinWidth: s->minWidth = resolveEm(d.value.length); break;
    case Prop::MinHeight: s->minHeight = resolveEm(d.value.length); break;
    case Prop::MaxWidth: s->maxWidth = resolveEm(d.value.length); break;
    case Prop::MaxHeight: s->maxHeight = resolveEm(d.value.length); break;
    case Prop::PaddingLeft: s->padding[0] = resolveEm(d.value.length); break;
    case Prop::PaddingTop: s->padding[1] = resolveEm(d.value.length); break;
    case Prop::PaddingRight: s->padding[2] = resolveEm(d.value.length); break;
    case Prop::PaddingBottom: s->padding[3] = resolveEm(d.value.length); break;
    case Prop::MarginLeft: s->margin[0] = resolveEm(d.value.length); break;
    case Prop::MarginTop: s->margin[1] = resolveEm(d.value.length); break;
    case Prop::MarginRight: s->margin[2] = resolveEm(d.value.length); break;
    case Prop::MarginBottom: s->margin[3] = resolveEm(d.value.length); break;
    case Prop::Left: s->inset[0] = resolveEm(d.value.length); break;
    case Prop::Top: s->inset[1] = resolveEm(d.value.length); break;
    case Prop::Right: s->inset[2] = resolveEm(d.value.length); break;
    case Prop::Bottom: s->inset[3] = resolveEm(d.value.length); break;
    case Prop::BorderWidth: s->borderWidth = d.value.number; break;
    case Prop::BorderColor: s->borderColor = d.value.color; break;
    case Prop::BorderRadiusTL: s->borderRadius.x = d.value.number; break;
    case Prop::BorderRadiusTR: s->borderRadius.y = d.value.number; break;
    case Prop::BorderRadiusBR: s->borderRadius.z = d.value.number; break;
    case Prop::BorderRadiusBL: s->borderRadius.w = d.value.number; break;
    case Prop::BackgroundColor: s->backgroundColor = d.value.color; break;
    case Prop::TextColor: s->textColor = d.value.color; break;
    case Prop::Opacity: s->opacity = d.value.number; break;
    case Prop::ShadowOffsetX: s->shadowOffset.x = d.value.number; break;
    case Prop::ShadowOffsetY: s->shadowOffset.y = d.value.number; break;
    case Prop::ShadowBlur: s->shadowBlur = d.value.number; break;
    case Prop::ShadowColor: s->shadowColor = d.value.color; break;
    case Prop::FontFamily: s->fontFamily = d.value.text; break;
    case Prop::LineHeight: s->lineHeight = d.value.number; break;
    // clang-format on
    case Prop::Transition: s->transitions = d.value.transitions; break;
    case Prop::Animation: s->animations = d.value.animations; break;
    case Prop::AnimationTiming:
        // Longhand on an element (a coordinated CSS list): timing i goes to
        // animation i, repeating cyclically when the list is shorter.
        // Inside @keyframes it never reaches the cascade — the parser
        // lifts it into Keyframe::timing.
        if (!d.value.animations.empty())
            for (size_t i = 0; i < s->animations.size(); ++i)
                s->animations[i].timing =
                    d.value.animations[i % d.value.animations.size()].timing;
        break;
    case Prop::FontSize:
        // em resolves against the *inherited* size (already in s->fontSize).
        if (d.value.length.unit == Unit::Em)
            s->fontSize = d.value.length.value * s->fontSize;
        else
            s->fontSize = d.value.length.value;
        break;
    case Prop::Count:
        break;
    }
}

void resolveStyle(const std::vector<const Stylesheet*>& sheets, const MatchContext& element,
                  const std::vector<Declaration>* inlineStyle, ComputedStyle* style) {
    std::vector<MatchedRule> matched;
    for (size_t sheetIndex = 0; sheetIndex < sheets.size(); ++sheetIndex) {
        const Stylesheet* sheet = sheets[sheetIndex];
        for (size_t ruleIndex = 0; ruleIndex < sheet->rules.size(); ++ruleIndex) {
            const Rule& rule = sheet->rules[ruleIndex];
            uint32_t best = 0;
            bool anyMatch = false;
            for (const ComplexSelector& selector : rule.selectors) {
                if (matchesSelector(selector, element)) {
                    anyMatch = true;
                    best = std::max(best, selector.specificity());
                }
            }
            if (anyMatch)
                matched.push_back(
                    {&rule, best,
                     static_cast<uint32_t>((sheetIndex << 16) | ruleIndex)});
        }
    }

    std::stable_sort(matched.begin(), matched.end(),
                     [](const MatchedRule& a, const MatchedRule& b) {
                         if (a.specificity != b.specificity)
                             return a.specificity < b.specificity;
                         return a.order < b.order;
                     });

    // font-size first within each source so em declarations resolve right.
    auto applyAll = [&](const std::vector<Declaration>& declarations) {
        for (const Declaration& d : declarations)
            if (d.prop == Prop::FontSize) applyDeclaration(d, style);
        for (const Declaration& d : declarations)
            if (d.prop != Prop::FontSize) applyDeclaration(d, style);
    };

    for (const MatchedRule& m : matched) applyAll(m.rule->declarations);
    if (inlineStyle != nullptr) applyAll(*inlineStyle);
}

} // namespace rendy::css
