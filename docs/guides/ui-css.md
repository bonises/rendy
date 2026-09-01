# UI & CSS

`rendy::ui` is a retained element tree: build it once, mutate it on events,
and it restyles/relayouts only when something changed. Styling is real CSS
(a well-defined subset) plus an equivalent typed C++ API — both produce the
same declarations, so semantics always agree.

```cpp
ui::Context ui(app);
ui.loadStylesheet("app.css");                 // hot-reloads in debug builds
auto row = ui.root().addChild("div", {.classes = "toolbar"});
row.addChild("button", {.text = "Open"}).onClick([](ui::Element e) { ... });

while (app.pollEvents()) {
    auto frame = app.beginFrame({});
    ui.update();               // input → hover/click/scroll, restyle, layout
    ui.paint(frame.canvas());  // paints through the 2D batch
    frame.present();
}
```

## Elements

`addChild(tag, {.classes, .id, .text})` — the tag is any string you like
(`"div"`, `"button"`, `"row"`, …); selectors match it. Handles are cheap
copies. Mutators: `setText`, `setClasses`, `addClass`, `removeClass`,
`setStyle` (inline typed style — wins the cascade), `onClick`,
`setDisabled`, `remove`, `clearChildren`. Read back: `text()`, `bounds()`,
`hovered()`.

Clicks bubble: a click on a child fires the nearest ancestor with an
`onClick` handler. Wheel scrolling finds the nearest `overflow: scroll`
ancestor with overflowing content.

## Supported CSS

**Selectors** — `tag`, `.class`, `#id`, `*`, compounds (`button.primary`),
descendant (`.panel button`), child (`.panel > .row`), and the
pseudo-classes `:hover :active :focus :disabled :first-child :last-child`.
Selector lists (`a, b { }`) work. Specificity and source order follow the
spec; later stylesheets override earlier ones; inline styles win.

**Layout properties** (flexbox via Yoga — browser-accurate):
`display: flex | none`, `flex-direction`, `flex-wrap`, `justify-content`,
`align-items/self/content`, `flex-grow/shrink/basis`, `flex` shorthand,
`gap`/`row-gap`/`column-gap`, `width/height`, `min-/max-width/height`,
`padding`/`margin` (1–4 value shorthands, `margin: auto` centers),
`position: relative | absolute` with `top/right/bottom/left`,
`overflow: visible | hidden | scroll`.

**Paint & text properties**: `background-color` (and `background` with a
color), `border` / `border-width` / `border-color`, `border-radius` (1–4
values), `color`, `opacity` (multiplies down the subtree), `font-size`,
`font-family`, `text-align`, `line-height` (number = multiplier, px =
absolute).

**Values**: `px`, `%`, `em` (relative to the element's font size; for
`font-size` itself, relative to the inherited size), unitless numbers where
CSS allows; colors as `#rgb #rrggbb #rrggbbaa`, `rgb()/rgba()`, ~20 named
colors, `transparent`.

Unknown properties log one warning and are ignored — a stylesheet never
fails to load because of them. Not in v1: media queries, `!important`,
attribute/sibling selectors, transitions/animations, text wrapping inside
elements.

Defaults differ from the browser where it helps UIs: every element is
`display: flex; flex-direction: column; align-items: stretch`, so vertical
stacking works with zero boilerplate.

## Fonts in CSS

Register loaded fonts under a family name, then use `font-family`:

```cpp
auto mono = app.loadFont(".../DejaVuSansMono.ttf").value();
ui.registerFont("mono", mono);
```

```css
.code { font-family: mono; font-size: 13px; }
```

## Hot reload

In debug builds every file loaded with `loadStylesheet` is watched (0.5 s
mtime poll); saving the file re-parses, restyles and relayouts live. String
sheets (`addStylesheet`) don't reload.

## Typed styles

Everything above exists as a builder, useful for computed values:

```cpp
element.setStyle(ui::Style{}
    .flexDirection(ui::FlexDirection::Row)
    .padding(8)
    .gap(12)
    .backgroundColor(Color::rgb(0x1E1E2E))
    .borderRadius(8));
```
