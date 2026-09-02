# ui

Headers: `rendy/ui/{ui,style}.hpp` — see the [UI & CSS guide](../guides/ui-css.md)
for the full CSS property/selector tables.

## Context

| Member | Signature | Notes |
|---|---|---|
| ctor | `Context(App&)` | |
| `loadStylesheet` | `Result<void> loadStylesheet(path)` | hot-reloads in debug |
| `addStylesheet` | `void addStylesheet(std::string_view css)` | no reload |
| `registerFont` | `void registerFont(name, FontRef)` | for `font-family: name` |
| `root` | `Element root()` | fills the window |
| `update` | `void update()` | input, restyle, layout — once per frame |
| `paint` | `void paint(Canvas)` | between beginFrame and present |

## Element

Cheap-copy handle; invalid after `remove()`.

| Member | Notes |
|---|---|
| `addChild(tag, {.classes, .id, .text})` | classes space-separated |
| `remove()` / `clearChildren()` | |
| `setText / setClasses / addClass / removeClass` | mark dirty only on change |
| `setStyle(const Style&)` | inline style, wins cascade |
| `onClick(fn)` | clicks bubble to nearest handler |
| `setDisabled(bool)` | drives `:disabled`, blocks clicks |
| `text() / bounds() / hovered() / valid()` | bounds = absolute px after layout |

## Style (typed builder)

Chainable setters mirroring the CSS properties:
`display flexDirection flexWrap justifyContent alignItems alignSelf
alignContent flexGrow flexShrink flexBasis gap rowGap columnGap width height
minWidth minHeight maxWidth maxHeight padding(1/2/4) margin(1/2/4) position
left top right bottom overflow borderWidth borderColor borderRadius
backgroundColor textColor opacity fontSize fontFamily textAlign lineHeight`,
plus `transition(Prop, seconds, Timing = Ease, delaySeconds = 0)` —
repeatable; `Prop::Count` means "all animatable" (see the UI guide).

Enums: `Display{Flex,None}`, `FlexDirection{Row,Column,RowReverse,ColumnReverse}`,
`FlexWrap{NoWrap,Wrap,WrapReverse}`, `Justify{FlexStart,FlexEnd,Center,
SpaceBetween,SpaceAround,SpaceEvenly}`, `Align{Auto,FlexStart,FlexEnd,Center,
Stretch,Baseline}`, `Position{Relative,Absolute}`, `Overflow{Visible,Hidden,
Scroll}`, `TextAlign{Left,Center,Right}`.

`Length`: `Length::px(v)`, `Length::percent(v)`, `Length::em(v)`,
`Length::autoValue()`.
