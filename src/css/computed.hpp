#pragma once

// The fully resolved style of one element after the cascade. Layout-shaped
// fields feed Yoga; paint-shaped fields feed the painter.

#include "rendy/math/math.hpp"
#include "rendy/ui/style.hpp"

namespace rendy::css {

struct ComputedStyle {
    using Length = ui::Length;

    // Layout.
    ui::Display display = ui::Display::Flex;
    ui::FlexDirection flexDirection = ui::FlexDirection::Column; // rendy default: column
    ui::FlexWrap flexWrap = ui::FlexWrap::NoWrap;
    ui::Justify justifyContent = ui::Justify::FlexStart;
    ui::Align alignItems = ui::Align::Stretch;
    ui::Align alignSelf = ui::Align::Auto;
    ui::Align alignContent = ui::Align::FlexStart;
    ui::Position position = ui::Position::Relative;
    ui::Overflow overflow = ui::Overflow::Visible;
    float flexGrow = 0.0f;
    float flexShrink = 1.0f;
    Length flexBasis = Length::autoValue();
    Length width = Length::autoValue();
    Length height = Length::autoValue();
    Length minWidth{}, minHeight{}, maxWidth{}, maxHeight{};
    Length padding[4]{}; ///< left, top, right, bottom
    Length margin[4]{};
    Length inset[4]{}; ///< left, top, right, bottom (position offsets)
    float rowGap = 0.0f;
    float columnGap = 0.0f;
    float borderWidth = 0.0f;

    // Paint.
    Color backgroundColor = colors::transparent;
    Color borderColor = colors::black;
    Vec4 borderRadius{0.0f}; ///< tl, tr, br, bl
    float opacity = 1.0f;

    // Inherited text properties.
    Color textColor = colors::white;
    float fontSize = 15.0f;
    std::string fontFamily;
    ui::TextAlign textAlign = ui::TextAlign::Left;
    float lineHeight = 0.0f; ///< >0 multiplier, <0 absolute px, 0 font default
};

} // namespace rendy::css
