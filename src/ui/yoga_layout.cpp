#include "ui/yoga_layout.hpp"

namespace rendy::ui {
namespace {

YGFlexDirection toYoga(FlexDirection v) {
    switch (v) {
    case FlexDirection::Row: return YGFlexDirectionRow;
    case FlexDirection::Column: return YGFlexDirectionColumn;
    case FlexDirection::RowReverse: return YGFlexDirectionRowReverse;
    case FlexDirection::ColumnReverse: return YGFlexDirectionColumnReverse;
    }
    return YGFlexDirectionColumn;
}

YGJustify toYoga(Justify v) {
    switch (v) {
    case Justify::FlexStart: return YGJustifyFlexStart;
    case Justify::FlexEnd: return YGJustifyFlexEnd;
    case Justify::Center: return YGJustifyCenter;
    case Justify::SpaceBetween: return YGJustifySpaceBetween;
    case Justify::SpaceAround: return YGJustifySpaceAround;
    case Justify::SpaceEvenly: return YGJustifySpaceEvenly;
    }
    return YGJustifyFlexStart;
}

YGAlign toYoga(Align v) {
    switch (v) {
    case Align::Auto: return YGAlignAuto;
    case Align::FlexStart: return YGAlignFlexStart;
    case Align::FlexEnd: return YGAlignFlexEnd;
    case Align::Center: return YGAlignCenter;
    case Align::Stretch: return YGAlignStretch;
    case Align::Baseline: return YGAlignBaseline;
    }
    return YGAlignAuto;
}

void setDimension(YGNodeRef node, Length v, void (*setPx)(YGNodeRef, float),
                  void (*setPercent)(YGNodeRef, float), void (*setAuto)(YGNodeRef)) {
    switch (v.unit) {
    case Unit::Px:
    case Unit::Em: setPx(node, v.value); break; // em resolved in cascade
    case Unit::Percent: setPercent(node, v.value); break;
    case Unit::Auto:
        if (setAuto != nullptr) setAuto(node);
        break;
    case Unit::None: break;
    }
}

void setEdge(YGNodeRef node, YGEdge edge, Length v,
             void (*setPx)(YGNodeRef, YGEdge, float),
             void (*setPercent)(YGNodeRef, YGEdge, float),
             void (*setAuto)(YGNodeRef, YGEdge)) {
    switch (v.unit) {
    case Unit::Px:
    case Unit::Em: setPx(node, edge, v.value); break;
    case Unit::Percent: setPercent(node, edge, v.value); break;
    case Unit::Auto:
        if (setAuto != nullptr) setAuto(node, edge);
        break;
    case Unit::None: break;
    }
}

} // namespace

void applyStyleToYoga(const css::ComputedStyle& s, YGNodeRef node) {
    YGNodeStyleSetDisplay(node, s.display == Display::None ? YGDisplayNone : YGDisplayFlex);
    YGNodeStyleSetFlexDirection(node, toYoga(s.flexDirection));
    YGNodeStyleSetFlexWrap(node, s.flexWrap == FlexWrap::NoWrap    ? YGWrapNoWrap
                                 : s.flexWrap == FlexWrap::Wrap    ? YGWrapWrap
                                                                   : YGWrapWrapReverse);
    YGNodeStyleSetJustifyContent(node, toYoga(s.justifyContent));
    YGNodeStyleSetAlignItems(node, toYoga(s.alignItems));
    YGNodeStyleSetAlignSelf(node, toYoga(s.alignSelf));
    YGNodeStyleSetAlignContent(node, toYoga(s.alignContent));
    YGNodeStyleSetPositionType(node, s.position == Position::Absolute
                                         ? YGPositionTypeAbsolute
                                         : YGPositionTypeRelative);
    YGNodeStyleSetOverflow(node, s.overflow == Overflow::Visible  ? YGOverflowVisible
                                 : s.overflow == Overflow::Hidden ? YGOverflowHidden
                                                                  : YGOverflowScroll);
    YGNodeStyleSetFlexGrow(node, s.flexGrow);
    YGNodeStyleSetFlexShrink(node, s.flexShrink);
    setDimension(node, s.flexBasis, YGNodeStyleSetFlexBasis, YGNodeStyleSetFlexBasisPercent,
                 YGNodeStyleSetFlexBasisAuto);

    setDimension(node, s.width, YGNodeStyleSetWidth, YGNodeStyleSetWidthPercent,
                 YGNodeStyleSetWidthAuto);
    setDimension(node, s.height, YGNodeStyleSetHeight, YGNodeStyleSetHeightPercent,
                 YGNodeStyleSetHeightAuto);
    setDimension(node, s.minWidth, YGNodeStyleSetMinWidth, YGNodeStyleSetMinWidthPercent,
                 nullptr);
    setDimension(node, s.minHeight, YGNodeStyleSetMinHeight, YGNodeStyleSetMinHeightPercent,
                 nullptr);
    setDimension(node, s.maxWidth, YGNodeStyleSetMaxWidth, YGNodeStyleSetMaxWidthPercent,
                 nullptr);
    setDimension(node, s.maxHeight, YGNodeStyleSetMaxHeight, YGNodeStyleSetMaxHeightPercent,
                 nullptr);

    const YGEdge edges[4] = {YGEdgeLeft, YGEdgeTop, YGEdgeRight, YGEdgeBottom};
    for (int i = 0; i < 4; ++i) {
        setEdge(node, edges[i], s.padding[i], YGNodeStyleSetPadding,
                YGNodeStyleSetPaddingPercent, nullptr);
        setEdge(node, edges[i], s.margin[i], YGNodeStyleSetMargin, YGNodeStyleSetMarginPercent,
                YGNodeStyleSetMarginAuto);
        setEdge(node, edges[i], s.inset[i], YGNodeStyleSetPosition,
                YGNodeStyleSetPositionPercent, nullptr);
    }

    YGNodeStyleSetGap(node, YGGutterRow, s.rowGap);
    YGNodeStyleSetGap(node, YGGutterColumn, s.columnGap);
    YGNodeStyleSetBorder(node, YGEdgeAll, s.borderWidth);
}

} // namespace rendy::ui
