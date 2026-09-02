#include "css/parser.hpp"

#include "css/tokenizer.hpp"
#include "rendy/core/log.hpp"

#include <fmt/core.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <optional>
#include <unordered_map>

namespace rendy::css {

using ui::Declaration;
using ui::Length;
using ui::Prop;
using ui::Unit;
using ui::Value;

namespace {

// The CSS cubic-bezier presets, shared by transition/animation parsing.
std::optional<ui::Timing> timingFromIdent(std::string_view ident) {
    static constexpr std::pair<std::string_view, ui::Timing> kTimings[] = {
        {"linear", ui::Timing::Linear},         {"ease", ui::Timing::Ease},
        {"ease-in", ui::Timing::EaseIn},        {"ease-out", ui::Timing::EaseOut},
        {"ease-in-out", ui::Timing::EaseInOut},
    };
    for (const auto& [keyword, timing] : kTimings)
        if (ident == keyword) return timing;
    return std::nullopt;
}

// ------------------------------------------------------------------ colors

const std::unordered_map<std::string_view, uint32_t> kNamedColors{
    {"black", 0x000000},   {"white", 0xFFFFFF},   {"red", 0xFF0000},
    {"green", 0x008000},   {"blue", 0x0000FF},    {"yellow", 0xFFFF00},
    {"orange", 0xFFA500},  {"purple", 0x800080},  {"gray", 0x808080},
    {"grey", 0x808080},    {"cyan", 0x00FFFF},    {"magenta", 0xFF00FF},
    {"pink", 0xFFC0CB},    {"brown", 0xA52A2A},   {"lime", 0x00FF00},
    {"navy", 0x000080},    {"teal", 0x008080},    {"olive", 0x808000},
    {"maroon", 0x800000},  {"silver", 0xC0C0C0},  {"gold", 0xFFD700},
};

bool parseHexColor(std::string_view hex, Color* out) {
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    uint32_t rgba = 0;
    if (hex.size() == 3 || hex.size() == 4) {
        for (char c : hex) {
            const int v = nibble(c);
            if (v < 0) return false;
            rgba = (rgba << 8) | static_cast<uint32_t>(v * 17);
        }
        if (hex.size() == 3) rgba = (rgba << 8) | 0xFF;
    } else if (hex.size() == 6 || hex.size() == 8) {
        for (char c : hex) {
            const int v = nibble(c);
            if (v < 0) return false;
            rgba = (rgba << 4) | static_cast<uint32_t>(v);
        }
        if (hex.size() == 6) rgba = (rgba << 8) | 0xFF;
    } else {
        return false;
    }
    *out = Color::rgba(rgba);
    return true;
}

float parseFloat(std::string_view text) {
    return std::strtof(std::string(text).c_str(), nullptr);
}

// ------------------------------------------------------------------ parser

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    Stylesheet run() {
        Stylesheet sheet;
        skipSpace();
        while (!peek().is(TokenType::End)) {
            if (peek().isDelim('@')) {
                parseAtRule(&sheet);
                continue;
            }
            Rule rule;
            if (parseRule(&rule, &sheet))
                sheet.rules.push_back(std::move(rule));
        }
        return sheet;
    }

private:
    const Token& peek(size_t ahead = 0) const {
        const size_t i = std::min(pos_ + ahead, tokens_.size() - 1);
        return tokens_[i];
    }
    const Token& next() {
        const Token& t = tokens_[std::min(pos_, tokens_.size() - 1)];
        if (pos_ < tokens_.size() - 1) pos_++;
        return t;
    }
    void skipSpace() {
        while (peek().is(TokenType::Whitespace)) pos_++;
    }
    /// Skip to just past the next matching `}` (error recovery).
    void skipBlock() {
        int depth = 0;
        while (!peek().is(TokenType::End)) {
            const Token& t = next();
            if (t.is(TokenType::LBrace)) depth++;
            if (t.is(TokenType::RBrace) && --depth <= 0) break;
        }
    }

    /// Skip an unknown at-rule: to `;` at top depth, or past a full block.
    void skipAtRule() {
        int depth = 0;
        while (!peek().is(TokenType::End)) {
            const Token& t = next();
            if (t.is(TokenType::Semicolon) && depth == 0) break;
            if (t.is(TokenType::LBrace)) depth++;
            if (t.is(TokenType::RBrace) && --depth <= 0) break;
        }
        skipSpace();
    }

    void parseAtRule(Stylesheet* sheet) {
        next(); // @
        if (!peek().is(TokenType::Ident)) {
            skipAtRule();
            return;
        }
        const std::string keyword = next().value;
        if (keyword == "keyframes") {
            parseKeyframes(sheet);
            return;
        }
        sheet->unsupported.push_back("@" + keyword);
        skipAtRule();
    }

    /// @keyframes <name> { 0%, 50% { ... } from { ... } to { ... } }
    void parseKeyframes(Stylesheet* sheet) {
        skipSpace();
        if (!peek().is(TokenType::Ident)) {
            skipAtRule();
            return;
        }
        KeyframesRule keyframes;
        keyframes.name = next().value;
        skipSpace();
        if (!peek().is(TokenType::LBrace)) {
            skipAtRule();
            return;
        }
        next(); // {
        skipSpace();
        while (!peek().is(TokenType::RBrace) && !peek().is(TokenType::End)) {
            // ---- offset list: percentages, `from` (0%) and `to` (100%)
            std::vector<float> offsets;
            bool ok = true;
            while (true) {
                skipSpace();
                const Token& t = peek();
                if (t.is(TokenType::Percentage)) {
                    offsets.push_back(parseFloat(next().value) / 100.0f);
                } else if (t.is(TokenType::Ident) && t.value == "from") {
                    next();
                    offsets.push_back(0.0f);
                } else if (t.is(TokenType::Ident) && t.value == "to") {
                    next();
                    offsets.push_back(1.0f);
                } else {
                    ok = false;
                    break;
                }
                skipSpace();
                if (peek().is(TokenType::Comma)) {
                    next();
                    continue;
                }
                break;
            }
            if (!ok || offsets.empty() || !peek().is(TokenType::LBrace)) {
                skipBlock(); // drop this malformed frame, keep the rest
                skipSpace();
                continue;
            }
            next(); // {
            Rule scratch;
            skipSpace();
            while (!peek().is(TokenType::RBrace) && !peek().is(TokenType::End)) {
                parseDeclaration(&scratch, sheet);
                skipSpace();
            }
            if (peek().is(TokenType::RBrace)) next();
            skipSpace();
            // animation-timing-function is per-frame metadata, not an
            // animated property — pull it out of the declaration list.
            bool hasTiming = false;
            ui::Timing timing = ui::Timing::Ease;
            std::erase_if(scratch.declarations, [&](const Declaration& d) {
                if (d.prop != Prop::AnimationTiming) return false;
                if (!d.value.animations.empty()) {
                    hasTiming = true;
                    timing = d.value.animations.front().timing;
                }
                return true;
            });
            for (float offset : offsets) {
                offset = std::clamp(offset, 0.0f, 1.0f);
                Keyframe* frame = nullptr;
                for (Keyframe& existing : keyframes.frames)
                    if (existing.offset == offset) frame = &existing;
                if (frame == nullptr) {
                    keyframes.frames.push_back({offset, {}});
                    frame = &keyframes.frames.back();
                }
                frame->declarations.insert(frame->declarations.end(),
                                           scratch.declarations.begin(),
                                           scratch.declarations.end());
                if (hasTiming) {
                    frame->hasTiming = true;
                    frame->timing = timing;
                }
            }
        }
        if (peek().is(TokenType::RBrace)) next();
        skipSpace();
        std::stable_sort(keyframes.frames.begin(), keyframes.frames.end(),
                         [](const Keyframe& a, const Keyframe& b) { return a.offset < b.offset; });
        if (!keyframes.frames.empty()) sheet->keyframes.push_back(std::move(keyframes));
    }

    bool parseRule(Rule* rule, Stylesheet* sheet) {
        // ---- selector list
        while (true) {
            ComplexSelector selector;
            if (!parseComplexSelector(&selector)) {
                skipBlock();
                skipSpace();
                return false;
            }
            rule->selectors.push_back(std::move(selector));
            skipSpace();
            if (peek().is(TokenType::Comma)) {
                next();
                skipSpace();
                continue;
            }
            break;
        }
        if (!peek().is(TokenType::LBrace)) {
            skipBlock();
            skipSpace();
            return false;
        }
        next(); // {

        // ---- declarations
        skipSpace();
        while (!peek().is(TokenType::RBrace) && !peek().is(TokenType::End)) {
            parseDeclaration(rule, sheet);
            skipSpace();
        }
        if (peek().is(TokenType::RBrace)) next();
        skipSpace();
        return !rule->selectors.empty();
    }

    bool parseCompound(SimpleSelector* out) {
        bool any = false;
        while (true) {
            const Token& t = peek();
            if (t.is(TokenType::Ident)) {
                out->tag = next().value;
                any = true;
            } else if (t.isDelim('*')) {
                next();
                any = true;
            } else if (t.is(TokenType::Hash)) {
                out->id = next().value;
                any = true;
            } else if (t.isDelim('.')) {
                next();
                if (!peek().is(TokenType::Ident)) return false;
                out->classes.push_back(next().value);
                any = true;
            } else if (t.is(TokenType::Colon)) {
                next();
                if (!peek().is(TokenType::Ident)) return false;
                const std::string pseudo = next().value;
                if (pseudo == "hover") out->pseudo |= kPseudoHover;
                else if (pseudo == "active") out->pseudo |= kPseudoActive;
                else if (pseudo == "focus") out->pseudo |= kPseudoFocus;
                else if (pseudo == "disabled") out->pseudo |= kPseudoDisabled;
                else if (pseudo == "first-child") out->pseudo |= kPseudoFirstChild;
                else if (pseudo == "last-child") out->pseudo |= kPseudoLastChild;
                else return false; // unsupported pseudo-class: drop the rule
                any = true;
            } else {
                break;
            }
        }
        return any;
    }

    bool parseComplexSelector(ComplexSelector* out) {
        SimpleSelector first;
        if (!parseCompound(&first)) return false;
        out->compounds.push_back(std::move(first));

        while (true) {
            const bool hadSpace = peek().is(TokenType::Whitespace);
            if (hadSpace) skipSpace();
            if (peek().isDelim('>')) {
                next();
                skipSpace();
                SimpleSelector compound;
                if (!parseCompound(&compound)) return false;
                out->combinators.push_back(Combinator::Child);
                out->compounds.push_back(std::move(compound));
                continue;
            }
            if (hadSpace && (peek().is(TokenType::Ident) || peek().is(TokenType::Hash) ||
                             peek().isDelim('.') || peek().isDelim('*') ||
                             peek().is(TokenType::Colon))) {
                SimpleSelector compound;
                if (!parseCompound(&compound)) return false;
                out->combinators.push_back(Combinator::Descendant);
                out->compounds.push_back(std::move(compound));
                continue;
            }
            return true;
        }
    }

    // ---- values ----------------------------------------------------------

    /// Collect the raw value tokens of one declaration (until ; or }).
    std::vector<Token> valueTokens() {
        std::vector<Token> out;
        while (!peek().is(TokenType::Semicolon) && !peek().is(TokenType::RBrace) &&
               !peek().is(TokenType::End)) {
            const Token& t = next();
            if (!t.is(TokenType::Whitespace)) out.push_back(t);
        }
        if (peek().is(TokenType::Semicolon)) next();
        return out;
    }

    static std::optional<Length> tokenToLength(const Token& t) {
        if (t.is(TokenType::Dimension)) {
            const float v = parseFloat(t.value);
            if (t.unit == "px") return Length::px(v);
            if (t.unit == "em") return Length::em(v);
            return std::nullopt;
        }
        if (t.is(TokenType::Percentage)) return Length::percent(parseFloat(t.value));
        if (t.is(TokenType::Number)) return Length::px(parseFloat(t.value)); // unitless = px
        if (t.is(TokenType::Ident) && t.value == "auto") return Length::autoValue();
        return std::nullopt;
    }

    static std::optional<Color> tokensToColor(const std::vector<Token>& tokens, size_t* index) {
        if (*index >= tokens.size()) return std::nullopt;
        const Token& t = tokens[*index];
        if (t.is(TokenType::Hash)) {
            Color c;
            if (parseHexColor(t.value, &c)) {
                (*index)++;
                return c;
            }
            return std::nullopt;
        }
        if (t.is(TokenType::Ident)) {
            if (t.value == "transparent") {
                (*index)++;
                return colors::transparent;
            }
            if (auto it = kNamedColors.find(t.value); it != kNamedColors.end()) {
                (*index)++;
                return Color::rgb(it->second);
            }
            return std::nullopt;
        }
        if (t.is(TokenType::Function) && (t.value == "rgb" || t.value == "rgba")) {
            std::array<float, 4> parts{0.0f, 0.0f, 0.0f, 1.0f};
            size_t i = *index + 1;
            size_t part = 0;
            while (i < tokens.size() && !tokens[i].is(TokenType::RParen) && part < 4) {
                if (tokens[i].is(TokenType::Number) || tokens[i].is(TokenType::Percentage)) {
                    float v = parseFloat(tokens[i].value);
                    if (tokens[i].is(TokenType::Percentage)) v = v / 100.0f * (part < 3 ? 255.0f : 1.0f);
                    parts[part++] = v;
                }
                i++;
            }
            while (i < tokens.size() && !tokens[i].is(TokenType::RParen)) i++;
            if (i < tokens.size()) i++; // ')'
            *index = i;
            return Color{parts[0] / 255.0f, parts[1] / 255.0f, parts[2] / 255.0f,
                         part == 4 ? parts[3] : 1.0f};
        }
        return std::nullopt;
    }

    // ---- declarations ----------------------------------------------------

    void emitLength(Rule* rule, Prop prop, Length v) {
        Declaration d{prop, {}};
        d.value.length = v;
        rule->declarations.push_back(std::move(d));
    }
    void emitNumber(Rule* rule, Prop prop, float v) {
        Declaration d{prop, {}};
        d.value.number = v;
        rule->declarations.push_back(std::move(d));
    }
    void emitKeyword(Rule* rule, Prop prop, uint8_t v) {
        Declaration d{prop, {}};
        d.value.keyword = v;
        rule->declarations.push_back(std::move(d));
    }
    void emitColor(Rule* rule, Prop prop, Color v) {
        Declaration d{prop, {}};
        d.value.color = v;
        rule->declarations.push_back(std::move(d));
    }

    /// Expand 1-4 box values (top right bottom left) to 4 props.
    bool emitBox(Rule* rule, const std::vector<Token>& tokens, Prop top, Prop right, Prop bottom,
                 Prop left) {
        std::vector<Length> values;
        for (const Token& t : tokens) {
            auto len = tokenToLength(t);
            if (!len) return false;
            values.push_back(*len);
        }
        if (values.empty() || values.size() > 4) return false;
        const Length t = values[0];
        const Length r = values.size() > 1 ? values[1] : t;
        const Length b = values.size() > 2 ? values[2] : t;
        const Length l = values.size() > 3 ? values[3] : r;
        emitLength(rule, top, t);
        emitLength(rule, right, r);
        emitLength(rule, bottom, b);
        emitLength(rule, left, l);
        return true;
    }

    template <typename Enum>
    bool emitEnum(Rule* rule, Prop prop, const std::vector<Token>& tokens,
                  std::initializer_list<std::pair<std::string_view, Enum>> table) {
        if (tokens.size() != 1 || !tokens[0].is(TokenType::Ident)) return false;
        for (const auto& [name, value] : table) {
            if (tokens[0].value == name) {
                emitKeyword(rule, prop, static_cast<uint8_t>(value));
                return true;
            }
        }
        return false;
    }

    bool applyDeclaration(Rule* rule, const std::string& name,
                          const std::vector<Token>& tokens);

    void parseDeclaration(Rule* rule, Stylesheet* sheet) {
        if (!peek().is(TokenType::Ident)) {
            // Garbage: skip to next ; or }.
            while (!peek().is(TokenType::Semicolon) && !peek().is(TokenType::RBrace) &&
                   !peek().is(TokenType::End))
                next();
            if (peek().is(TokenType::Semicolon)) next();
            return;
        }
        const std::string name = next().value;
        skipSpace();
        if (!peek().is(TokenType::Colon)) return;
        next();
        skipSpace();
        const std::vector<Token> value = valueTokens();
        if (!applyDeclaration(rule, name, value)) sheet->unsupported.push_back(name);
    }

    std::vector<Token> tokens_;
    size_t pos_ = 0;
};

bool Parser::applyDeclaration(Rule* rule, const std::string& name,
                              const std::vector<Token>& v) {
    using ui::Align;
    using ui::Display;
    using ui::FlexDirection;
    using ui::FlexWrap;
    using ui::Justify;
    using ui::Overflow;
    using ui::Position;
    using ui::TextAlign;

    auto single = [&]() -> std::optional<Length> {
        return v.size() == 1 ? tokenToLength(v[0]) : std::nullopt;
    };
    auto singleNumber = [&]() -> std::optional<float> {
        if (v.size() == 1 && (v[0].is(TokenType::Number) || v[0].is(TokenType::Dimension)))
            return parseFloat(v[0].value);
        return std::nullopt;
    };
    auto singleColor = [&]() -> std::optional<Color> {
        size_t i = 0;
        auto c = tokensToColor(v, &i);
        return (c && i == v.size()) ? c : std::nullopt;
    };

    // clang-format off
    if (name == "display")
        return emitEnum<Display>(rule, Prop::Display, v, {{"flex", Display::Flex}, {"none", Display::None}});
    if (name == "flex-direction")
        return emitEnum<FlexDirection>(rule, Prop::FlexDirection, v, {{"row", FlexDirection::Row}, {"column", FlexDirection::Column}, {"row-reverse", FlexDirection::RowReverse}, {"column-reverse", FlexDirection::ColumnReverse}});
    if (name == "flex-wrap")
        return emitEnum<FlexWrap>(rule, Prop::FlexWrap, v, {{"nowrap", FlexWrap::NoWrap}, {"wrap", FlexWrap::Wrap}, {"wrap-reverse", FlexWrap::WrapReverse}});
    if (name == "justify-content")
        return emitEnum<Justify>(rule, Prop::JustifyContent, v, {{"flex-start", Justify::FlexStart}, {"start", Justify::FlexStart}, {"flex-end", Justify::FlexEnd}, {"end", Justify::FlexEnd}, {"center", Justify::Center}, {"space-between", Justify::SpaceBetween}, {"space-around", Justify::SpaceAround}, {"space-evenly", Justify::SpaceEvenly}});
    if (name == "align-items" || name == "align-self" || name == "align-content") {
        const Prop p = name == "align-items" ? Prop::AlignItems : name == "align-self" ? Prop::AlignSelf : Prop::AlignContent;
        return emitEnum<Align>(rule, p, v, {{"auto", Align::Auto}, {"flex-start", Align::FlexStart}, {"start", Align::FlexStart}, {"flex-end", Align::FlexEnd}, {"end", Align::FlexEnd}, {"center", Align::Center}, {"stretch", Align::Stretch}, {"baseline", Align::Baseline}});
    }
    if (name == "position")
        return emitEnum<Position>(rule, Prop::PositionType, v, {{"relative", Position::Relative}, {"absolute", Position::Absolute}});
    if (name == "overflow")
        return emitEnum<Overflow>(rule, Prop::Overflow, v, {{"visible", Overflow::Visible}, {"hidden", Overflow::Hidden}, {"scroll", Overflow::Scroll}});
    if (name == "text-align")
        return emitEnum<TextAlign>(rule, Prop::TextAlignProp, v, {{"left", TextAlign::Left}, {"center", TextAlign::Center}, {"right", TextAlign::Right}});
    // clang-format on

    if (name == "flex-grow") {
        auto n = singleNumber();
        if (!n) return false;
        emitNumber(rule, Prop::FlexGrow, *n);
        return true;
    }
    if (name == "flex-shrink") {
        auto n = singleNumber();
        if (!n) return false;
        emitNumber(rule, Prop::FlexShrink, *n);
        return true;
    }
    if (name == "flex-basis") {
        auto len = single();
        if (!len) return false;
        emitLength(rule, Prop::FlexBasis, *len);
        return true;
    }
    if (name == "flex") {
        // flex: <grow> [<shrink>] [<basis>] | none
        if (v.size() == 1 && v[0].is(TokenType::Ident) && v[0].value == "none") {
            emitNumber(rule, Prop::FlexGrow, 0.0f);
            emitNumber(rule, Prop::FlexShrink, 0.0f);
            emitLength(rule, Prop::FlexBasis, Length::autoValue());
            return true;
        }
        if (v.empty() || !v[0].is(TokenType::Number)) return false;
        emitNumber(rule, Prop::FlexGrow, parseFloat(v[0].value));
        emitNumber(rule, Prop::FlexShrink, v.size() > 1 && v[1].is(TokenType::Number)
                                               ? parseFloat(v[1].value)
                                               : 1.0f);
        Length basis = Length::px(0.0f);
        if (v.size() > 1) {
            if (auto len = tokenToLength(v.back());
                len && !v.back().is(TokenType::Number)) basis = *len;
        }
        emitLength(rule, Prop::FlexBasis, basis);
        return true;
    }
    if (name == "gap" || name == "row-gap" || name == "column-gap") {
        if (v.empty() || v.size() > 2) return false;
        auto first = tokenToLength(v[0]);
        if (!first || first->unit != Unit::Px) return false;
        if (name == "row-gap") {
            emitNumber(rule, Prop::RowGap, first->value);
            return true;
        }
        if (name == "column-gap") {
            emitNumber(rule, Prop::ColumnGap, first->value);
            return true;
        }
        float column = first->value;
        if (v.size() == 2) {
            auto second = tokenToLength(v[1]);
            if (!second || second->unit != Unit::Px) return false;
            column = second->value;
        }
        emitNumber(rule, Prop::RowGap, first->value);
        emitNumber(rule, Prop::ColumnGap, column);
        return true;
    }

    static const std::unordered_map<std::string_view, Prop> kLengthProps{
        {"width", Prop::Width},           {"height", Prop::Height},
        {"min-width", Prop::MinWidth},    {"min-height", Prop::MinHeight},
        {"max-width", Prop::MaxWidth},    {"max-height", Prop::MaxHeight},
        {"left", Prop::Left},             {"top", Prop::Top},
        {"right", Prop::Right},           {"bottom", Prop::Bottom},
        {"padding-left", Prop::PaddingLeft},   {"padding-top", Prop::PaddingTop},
        {"padding-right", Prop::PaddingRight}, {"padding-bottom", Prop::PaddingBottom},
        {"margin-left", Prop::MarginLeft},     {"margin-top", Prop::MarginTop},
        {"margin-right", Prop::MarginRight},   {"margin-bottom", Prop::MarginBottom},
    };
    if (auto it = kLengthProps.find(name); it != kLengthProps.end()) {
        auto len = single();
        if (!len) return false;
        emitLength(rule, it->second, *len);
        return true;
    }

    if (name == "padding")
        return emitBox(rule, v, Prop::PaddingTop, Prop::PaddingRight, Prop::PaddingBottom,
                       Prop::PaddingLeft);
    if (name == "margin")
        return emitBox(rule, v, Prop::MarginTop, Prop::MarginRight, Prop::MarginBottom,
                       Prop::MarginLeft);

    if (name == "border-width") {
        auto n = singleNumber();
        if (!n) return false;
        emitNumber(rule, Prop::BorderWidth, *n);
        return true;
    }
    if (name == "border-color") {
        auto c = singleColor();
        if (!c) return false;
        emitColor(rule, Prop::BorderColor, *c);
        return true;
    }
    if (name == "border") {
        // border: <width> [solid] <color>
        float width = 1.0f;
        Color color = colors::black;
        bool haveColor = false;
        size_t i = 0;
        while (i < v.size()) {
            if (v[i].is(TokenType::Dimension) || v[i].is(TokenType::Number)) {
                width = parseFloat(v[i].value);
                i++;
                continue;
            }
            if (v[i].is(TokenType::Ident) &&
                (v[i].value == "solid" || v[i].value == "none")) {
                if (v[i].value == "none") width = 0.0f;
                i++;
                continue;
            }
            if (auto c = tokensToColor(v, &i)) {
                color = *c;
                haveColor = true;
                continue;
            }
            return false;
        }
        emitNumber(rule, Prop::BorderWidth, width);
        if (haveColor) emitColor(rule, Prop::BorderColor, color);
        return true;
    }
    if (name == "border-radius") {
        std::vector<float> values;
        for (const Token& t : v) {
            if (!t.is(TokenType::Dimension) && !t.is(TokenType::Number)) return false;
            values.push_back(parseFloat(t.value));
        }
        if (values.empty() || values.size() > 4) return false;
        const float tl = values[0];
        const float tr = values.size() > 1 ? values[1] : tl;
        const float br = values.size() > 2 ? values[2] : tl;
        const float bl = values.size() > 3 ? values[3] : tr;
        emitNumber(rule, Prop::BorderRadiusTL, tl);
        emitNumber(rule, Prop::BorderRadiusTR, tr);
        emitNumber(rule, Prop::BorderRadiusBR, br);
        emitNumber(rule, Prop::BorderRadiusBL, bl);
        return true;
    }

    if (name == "background-color" || name == "background") {
        auto c = singleColor();
        if (!c) return false;
        emitColor(rule, Prop::BackgroundColor, *c);
        return true;
    }
    if (name == "color") {
        auto c = singleColor();
        if (!c) return false;
        emitColor(rule, Prop::TextColor, *c);
        return true;
    }
    if (name == "opacity") {
        auto n = singleNumber();
        if (!n) return false;
        emitNumber(rule, Prop::Opacity, *n);
        return true;
    }
    if (name == "font-size") {
        auto len = single();
        if (!len || (len->unit != Unit::Px && len->unit != Unit::Em)) return false;
        Declaration d{Prop::FontSize, {}};
        d.value.length = *len;
        rule->declarations.push_back(std::move(d));
        return true;
    }
    if (name == "font-family") {
        // Take the first family name; quoted or bare ident.
        if (v.empty()) return false;
        std::string family;
        if (v[0].is(TokenType::String) || v[0].is(TokenType::Ident)) family = v[0].value;
        else return false;
        Declaration d{Prop::FontFamily, {}};
        d.value.text = std::move(family);
        rule->declarations.push_back(std::move(d));
        return true;
    }
    if (name == "line-height") {
        // Number = multiplier; px = absolute (stored negative to distinguish).
        if (v.size() != 1) return false;
        if (v[0].is(TokenType::Number)) {
            emitNumber(rule, Prop::LineHeight, parseFloat(v[0].value));
            return true;
        }
        if (v[0].is(TokenType::Dimension) && v[0].unit == "px") {
            emitNumber(rule, Prop::LineHeight, -parseFloat(v[0].value));
            return true;
        }
        return false;
    }
    if (name == "box-shadow") {
        // box-shadow: none | <x> <y> <blur> [<color>]  (single, no spread/inset)
        if (v.size() == 1 && v[0].is(TokenType::Ident) && v[0].value == "none") {
            emitNumber(rule, Prop::ShadowBlur, 0.0f);
            return true;
        }
        std::vector<float> lengths;
        size_t i = 0;
        while (i < v.size() && lengths.size() < 3 &&
               (v[i].is(TokenType::Dimension) || v[i].is(TokenType::Number))) {
            lengths.push_back(parseFloat(v[i].value));
            ++i;
        }
        if (lengths.size() < 3) return false;
        emitNumber(rule, Prop::ShadowOffsetX, lengths[0]);
        emitNumber(rule, Prop::ShadowOffsetY, lengths[1]);
        emitNumber(rule, Prop::ShadowBlur, lengths[2]);
        if (i < v.size()) {
            auto c = tokensToColor(v, &i);
            if (!c || i != v.size()) return false;
            emitColor(rule, Prop::ShadowColor, *c);
        }
        return true;
    }
    if (name == "transition") {
        // transition: <property> <duration> [<timing>] [<delay>] {, ...}
        // "none" clears; property "all" (or omitted) animates everything.
        Declaration d{Prop::Transition, {}};
        if (v.size() == 1 && v[0].is(TokenType::Ident) && v[0].value == "none") {
            rule->declarations.push_back(std::move(d));
            return true;
        }
        static constexpr std::pair<std::string_view, Prop> kAnimatable[] = {
            {"all", Prop::Count},
            {"background-color", Prop::BackgroundColor},
            {"background", Prop::BackgroundColor},
            {"color", Prop::TextColor},
            {"border-color", Prop::BorderColor},
            {"border-width", Prop::BorderWidth},
            {"border-radius", Prop::BorderRadiusTL}, // expanded on start
            {"box-shadow", Prop::ShadowBlur},         // expanded on start
            {"opacity", Prop::Opacity},
            {"width", Prop::Width},
            {"height", Prop::Height},
        };
        auto seconds = [](const Token& t) -> std::optional<float> {
            if (!t.is(TokenType::Dimension)) return std::nullopt;
            if (t.unit == "s") return parseFloat(t.value);
            if (t.unit == "ms") return parseFloat(t.value) / 1000.0f;
            return std::nullopt;
        };
        size_t i = 0;
        while (i < v.size()) {
            ui::TransitionSpec spec;
            bool haveDuration = false;
            bool haveDelay = false;
            for (; i < v.size() && !v[i].is(TokenType::Comma); ++i) {
                if (auto s = seconds(v[i])) {
                    if (!haveDuration) {
                        if (*s < 0.0f) return false; // CSS: negative duration is
                                                     // invalid (negative delay is fine)
                        spec.duration = *s;
                        haveDuration = true;
                    } else if (!haveDelay) {
                        spec.delay = *s;
                        haveDelay = true;
                    } else {
                        return false; // a third time value is invalid
                    }
                    continue;
                }
                if (!v[i].is(TokenType::Ident)) return false;
                bool matched = false;
                if (auto timing = timingFromIdent(v[i].value)) {
                    spec.timing = *timing;
                    matched = true;
                }
                if (matched) continue;
                for (const auto& [ident, prop] : kAnimatable) {
                    if (v[i].value == ident) {
                        spec.prop = prop;
                        matched = true;
                        break;
                    }
                }
                if (!matched) return false; // unknown/unanimatable property
            }
            if (!haveDuration) return false;
            d.value.transitions.push_back(spec);
            if (i < v.size()) ++i; // skip the comma
        }
        if (d.value.transitions.empty()) return false;
        rule->declarations.push_back(std::move(d));
        return true;
    }
    if (name == "animation-timing-function") {
        // List-valued per CSS (`<easing-function>#`), coordinated with the
        // element's animation list: value i applies to animation i, the
        // list repeating cyclically when it is shorter. Inside a
        // `@keyframes` block only the first value is used (the keyframes
        // are shared by every animation referencing them) and it eases the
        // segment starting at that keyframe. Carried as timing-only
        // AnimationSpec entries.
        Declaration d{Prop::AnimationTiming, {}};
        size_t i = 0;
        while (i < v.size()) {
            if (!v[i].is(TokenType::Ident)) return false;
            const auto timing = timingFromIdent(v[i].value);
            if (!timing) return false;
            ui::AnimationSpec spec;
            spec.timing = *timing;
            d.value.animations.push_back(spec);
            ++i;
            if (i < v.size()) {
                if (!v[i].is(TokenType::Comma)) return false;
                ++i;
                if (i >= v.size()) return false; // trailing comma
            }
        }
        if (d.value.animations.empty()) return false;
        rule->declarations.push_back(std::move(d));
        return true;
    }
    if (name == "animation") {
        // animation: <name> <duration> [<timing>] [<delay>] [<count>|infinite]
        //            [<direction>] [forwards|none] {, ...}
        // Keywords are claimed before the name (like CSS), so an animation
        // can't be named e.g. "reverse".
        Declaration d{Prop::Animation, {}};
        if (v.size() == 1 && v[0].is(TokenType::Ident) && v[0].value == "none") {
            rule->declarations.push_back(std::move(d));
            return true;
        }
        static constexpr std::pair<std::string_view, ui::AnimDirection> kDirections[] = {
            {"normal", ui::AnimDirection::Normal},
            {"reverse", ui::AnimDirection::Reverse},
            {"alternate", ui::AnimDirection::Alternate},
            {"alternate-reverse", ui::AnimDirection::AlternateReverse},
        };
        auto seconds = [](const Token& t) -> std::optional<float> {
            if (!t.is(TokenType::Dimension)) return std::nullopt;
            if (t.unit == "s") return parseFloat(t.value);
            if (t.unit == "ms") return parseFloat(t.value) / 1000.0f;
            return std::nullopt;
        };
        size_t i = 0;
        while (i < v.size()) {
            ui::AnimationSpec spec;
            bool haveDuration = false;
            bool haveDelay = false;
            bool haveName = false;
            bool haveTiming = false;
            bool haveIterations = false;
            bool haveDirection = false;
            for (; i < v.size() && !v[i].is(TokenType::Comma); ++i) {
                if (auto s = seconds(v[i])) {
                    if (!haveDuration) {
                        if (*s < 0.0f) return false; // negative duration is invalid
                        spec.duration = *s;
                        haveDuration = true;
                    } else if (!haveDelay) {
                        spec.delay = *s;
                        haveDelay = true;
                    } else {
                        return false;
                    }
                    continue;
                }
                if (v[i].is(TokenType::Number)) {
                    const float n = parseFloat(v[i].value);
                    if (haveIterations || n < 0.0f) return false;
                    spec.iterations = n;
                    haveIterations = true;
                    continue;
                }
                if (!v[i].is(TokenType::Ident)) return false;
                const std::string& ident = v[i].value;
                if (ident == "infinite") {
                    if (haveIterations) return false;
                    spec.iterations = std::numeric_limits<float>::infinity();
                    haveIterations = true;
                    continue;
                }
                bool matched = false;
                if (!haveTiming) {
                    if (auto timing = timingFromIdent(ident)) {
                        spec.timing = *timing;
                        haveTiming = matched = true;
                    }
                }
                if (matched) continue;
                if (!haveDirection) {
                    for (const auto& [keyword, direction] : kDirections) {
                        if (ident == keyword) {
                            spec.direction = direction;
                            haveDirection = matched = true;
                            break;
                        }
                    }
                }
                if (matched) continue;
                if (ident == "forwards" || ident == "both") {
                    spec.fillForwards = true;
                    continue;
                }
                if (ident == "backwards") continue; // no backwards fill: ignored
                if (!haveName) {
                    spec.name = ident;
                    haveName = true;
                    continue;
                }
                return false; // second unclaimed ident
            }
            if (!haveName || !haveDuration) return false;
            d.value.animations.push_back(std::move(spec));
            if (i < v.size()) ++i; // skip the comma
        }
        if (d.value.animations.empty()) return false;
        rule->declarations.push_back(std::move(d));
        return true;
    }
    return false;
}

} // namespace

Result<Stylesheet> parse(std::string_view cssText) {
    Parser parser(tokenize(cssText));
    Stylesheet sheet = parser.run();
    for (const std::string& name : sheet.unsupported)
        log::warn("css: unsupported or invalid property '{}' ignored", name);
    return sheet;
}

bool parseColorText(std::string_view text, Color* out) {
    // Route through the declaration parser so tests exercise the real path.
    auto sheet = parse(fmt::format("x {{ color: {}; }}", text));
    if (!sheet) return false;
    for (const Rule& rule : sheet.value().rules)
        for (const Declaration& d : rule.declarations)
            if (d.prop == Prop::TextColor) {
                *out = d.value.color;
                return true;
            }
    return false;
}

} // namespace rendy::css
