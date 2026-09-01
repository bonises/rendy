#pragma once

/// \file style.hpp
/// Styling vocabulary shared by the CSS engine and the typed C++ API. Both
/// produce the same Declarations, so `.css` files and Style builders always
/// agree on semantics.

#include "../core/color.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace rendy::ui {

enum class Display : uint8_t { Flex, None };
enum class FlexDirection : uint8_t { Row, Column, RowReverse, ColumnReverse };
enum class FlexWrap : uint8_t { NoWrap, Wrap, WrapReverse };
enum class Justify : uint8_t { FlexStart, FlexEnd, Center, SpaceBetween, SpaceAround, SpaceEvenly };
enum class Align : uint8_t { Auto, FlexStart, FlexEnd, Center, Stretch, Baseline };
enum class Position : uint8_t { Relative, Absolute };
enum class Overflow : uint8_t { Visible, Hidden, Scroll };
enum class TextAlign : uint8_t { Left, Center, Right };

enum class Unit : uint8_t { None, Px, Percent, Em, Auto };

struct Length {
    float value = 0.0f;
    Unit unit = Unit::None; ///< None = unset/initial

    static constexpr Length px(float v) { return {v, Unit::Px}; }
    static constexpr Length percent(float v) { return {v, Unit::Percent}; }
    static constexpr Length em(float v) { return {v, Unit::Em}; }
    static constexpr Length autoValue() { return {0.0f, Unit::Auto}; }
    [[nodiscard]] bool isSet() const { return unit != Unit::None; }

    friend constexpr bool operator==(const Length&, const Length&) = default;
};

/// Every styleable property. Shorthands expand to these during parsing.
enum class Prop : uint8_t {
    // clang-format off
    Display, FlexDirection, FlexWrap, JustifyContent, AlignItems, AlignSelf, AlignContent,
    FlexGrow, FlexShrink, FlexBasis, RowGap, ColumnGap,
    Width, Height, MinWidth, MinHeight, MaxWidth, MaxHeight,
    PaddingLeft, PaddingTop, PaddingRight, PaddingBottom,
    MarginLeft, MarginTop, MarginRight, MarginBottom,
    PositionType, Left, Top, Right, Bottom,
    Overflow, BorderWidth, BorderColor,
    BorderRadiusTL, BorderRadiusTR, BorderRadiusBR, BorderRadiusBL,
    BackgroundColor, TextColor, Opacity,
    FontSize, FontFamily, TextAlignProp, LineHeight,
    Count,
    // clang-format on
};

/// A property value; which member is meaningful depends on the property.
struct Value {
    Length length{};
    Color color{};
    float number = 0.0f;
    uint8_t keyword = 0;
    std::string text; ///< font-family only
};

struct Declaration {
    Prop prop{};
    Value value{};
};

/// Typed styling: `Style{}.flexDirection(FlexDirection::Row).padding(8)`.
/// Applied to an element it behaves like an inline style (wins the cascade).
class Style {
public:
    // clang-format off
    Style& display(Display v) { return keyword(Prop::Display, static_cast<uint8_t>(v)); }
    Style& flexDirection(FlexDirection v) { return keyword(Prop::FlexDirection, static_cast<uint8_t>(v)); }
    Style& flexWrap(FlexWrap v) { return keyword(Prop::FlexWrap, static_cast<uint8_t>(v)); }
    Style& justifyContent(Justify v) { return keyword(Prop::JustifyContent, static_cast<uint8_t>(v)); }
    Style& alignItems(Align v) { return keyword(Prop::AlignItems, static_cast<uint8_t>(v)); }
    Style& alignSelf(Align v) { return keyword(Prop::AlignSelf, static_cast<uint8_t>(v)); }
    Style& alignContent(Align v) { return keyword(Prop::AlignContent, static_cast<uint8_t>(v)); }
    Style& flexGrow(float v) { return number(Prop::FlexGrow, v); }
    Style& flexShrink(float v) { return number(Prop::FlexShrink, v); }
    Style& flexBasis(Length v) { return length(Prop::FlexBasis, v); }
    Style& gap(float px) { number(Prop::RowGap, px); return number(Prop::ColumnGap, px); }
    Style& rowGap(float px) { return number(Prop::RowGap, px); }
    Style& columnGap(float px) { return number(Prop::ColumnGap, px); }
    Style& width(Length v) { return length(Prop::Width, v); }
    Style& height(Length v) { return length(Prop::Height, v); }
    Style& minWidth(Length v) { return length(Prop::MinWidth, v); }
    Style& minHeight(Length v) { return length(Prop::MinHeight, v); }
    Style& maxWidth(Length v) { return length(Prop::MaxWidth, v); }
    Style& maxHeight(Length v) { return length(Prop::MaxHeight, v); }
    Style& padding(float px) { return padding(px, px, px, px); }
    Style& padding(float vertical, float horizontal) { return padding(vertical, horizontal, vertical, horizontal); }
    Style& padding(float top, float right, float bottom, float left) {
        length(Prop::PaddingTop, Length::px(top)); length(Prop::PaddingRight, Length::px(right));
        length(Prop::PaddingBottom, Length::px(bottom)); return length(Prop::PaddingLeft, Length::px(left));
    }
    Style& margin(float px) { return margin(px, px, px, px); }
    Style& margin(float vertical, float horizontal) { return margin(vertical, horizontal, vertical, horizontal); }
    Style& margin(float top, float right, float bottom, float left) {
        length(Prop::MarginTop, Length::px(top)); length(Prop::MarginRight, Length::px(right));
        length(Prop::MarginBottom, Length::px(bottom)); return length(Prop::MarginLeft, Length::px(left));
    }
    Style& position(Position v) { return keyword(Prop::PositionType, static_cast<uint8_t>(v)); }
    Style& left(Length v) { return length(Prop::Left, v); }
    Style& top(Length v) { return length(Prop::Top, v); }
    Style& right(Length v) { return length(Prop::Right, v); }
    Style& bottom(Length v) { return length(Prop::Bottom, v); }
    Style& overflow(Overflow v) { return keyword(Prop::Overflow, static_cast<uint8_t>(v)); }
    Style& borderWidth(float px) { return number(Prop::BorderWidth, px); }
    Style& borderColor(Color c) { return color(Prop::BorderColor, c); }
    Style& borderRadius(float px) {
        number(Prop::BorderRadiusTL, px); number(Prop::BorderRadiusTR, px);
        number(Prop::BorderRadiusBR, px); return number(Prop::BorderRadiusBL, px);
    }
    Style& backgroundColor(Color c) { return color(Prop::BackgroundColor, c); }
    Style& textColor(Color c) { return color(Prop::TextColor, c); }
    Style& opacity(float v) { return number(Prop::Opacity, v); }
    Style& fontSize(float px) { return number(Prop::FontSize, px); }
    Style& fontFamily(std::string name) {
        Declaration d{Prop::FontFamily, {}};
        d.value.text = std::move(name);
        declarations_.push_back(std::move(d));
        return *this;
    }
    Style& textAlign(TextAlign v) { return keyword(Prop::TextAlignProp, static_cast<uint8_t>(v)); }
    Style& lineHeight(float factor) { return number(Prop::LineHeight, factor); }
    // clang-format on

    [[nodiscard]] const std::vector<Declaration>& declarations() const { return declarations_; }

private:
    Style& keyword(Prop p, uint8_t v) {
        Declaration d{p, {}};
        d.value.keyword = v;
        declarations_.push_back(std::move(d));
        return *this;
    }
    Style& number(Prop p, float v) {
        Declaration d{p, {}};
        d.value.number = v;
        declarations_.push_back(std::move(d));
        return *this;
    }
    Style& length(Prop p, Length v) {
        Declaration d{p, {}};
        d.value.length = v;
        declarations_.push_back(std::move(d));
        return *this;
    }
    Style& color(Prop p, Color v) {
        Declaration d{p, {}};
        d.value.color = v;
        declarations_.push_back(std::move(d));
        return *this;
    }

    std::vector<Declaration> declarations_;
};

} // namespace rendy::ui
