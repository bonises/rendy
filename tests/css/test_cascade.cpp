#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "css/cascade.hpp"
#include "css/parser.hpp"

using Catch::Approx;
using namespace rendy;
using namespace rendy::css;

namespace {

Stylesheet sheetOf(std::string_view text) {
    auto result = parse(text);
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

ComputedStyle resolve(const std::vector<const Stylesheet*>& sheets, const MatchContext& ctx) {
    ComputedStyle style;
    resolveStyle(sheets, ctx, nullptr, &style);
    return style;
}

} // namespace

TEST_CASE("type, class, id matching", "[css][cascade]") {
    auto sheet = sheetOf("div { opacity: 0.1; } .a { opacity: 0.2; } #x { opacity: 0.3; }");
    std::vector<std::string> classes{"a"};

    MatchContext div{"div", "", nullptr, 0, nullptr};
    REQUIRE(resolve({&sheet}, div).opacity == Approx(0.1f));

    MatchContext withClass{"span", "", &classes, 0, nullptr};
    REQUIRE(resolve({&sheet}, withClass).opacity == Approx(0.2f));

    MatchContext withId{"span", "x", nullptr, 0, nullptr};
    REQUIRE(resolve({&sheet}, withId).opacity == Approx(0.3f));
}

TEST_CASE("specificity: id > class > type; later source wins ties", "[css][cascade]") {
    auto sheet = sheetOf("#x { opacity: 0.9; } div.a { opacity: 0.5; } div { opacity: 0.1; } "
                         "div { opacity: 0.2; }");
    std::vector<std::string> classes{"a"};

    // Type only: the later of the two div rules wins.
    MatchContext div{"div", "", nullptr, 0, nullptr};
    REQUIRE(resolve({&sheet}, div).opacity == Approx(0.2f));

    // Class beats type regardless of order.
    MatchContext divClass{"div", "", &classes, 0, nullptr};
    REQUIRE(resolve({&sheet}, divClass).opacity == Approx(0.5f));

    // Id beats both.
    MatchContext divId{"div", "x", &classes, 0, nullptr};
    REQUIRE(resolve({&sheet}, divId).opacity == Approx(0.9f));
}

TEST_CASE("descendant and child combinators", "[css][cascade]") {
    auto sheet = sheetOf(".panel button { opacity: 0.4; } .panel > .row { opacity: 0.6; }");
    std::vector<std::string> panelClass{"panel"};
    std::vector<std::string> rowClass{"row"};

    MatchContext panel{"div", "", &panelClass, 0, nullptr};
    MatchContext middle{"div", "", nullptr, 0, &panel};
    MatchContext button{"button", "", nullptr, 0, &middle};

    // Descendant matches through intermediate nodes.
    REQUIRE(resolve({&sheet}, button).opacity == Approx(0.4f));

    // Child only matches direct children.
    MatchContext rowDeep{"div", "", &rowClass, 0, &middle};
    REQUIRE(resolve({&sheet}, rowDeep).opacity == Approx(1.0f));
    MatchContext rowDirect{"div", "", &rowClass, 0, &panel};
    REQUIRE(resolve({&sheet}, rowDirect).opacity == Approx(0.6f));
}

TEST_CASE("mixed child/descendant combinators backtrack", "[css][cascade]") {
    // .a > .b .c — the NEAREST .b ancestor has no .a parent, but a higher
    // .b does; greedy matching would miss it.
    auto sheet = sheetOf(".a > .b .c { opacity: 0.3; }");
    std::vector<std::string> a{"a"};
    std::vector<std::string> b{"b"};
    std::vector<std::string> c{"c"};

    MatchContext top{"div", "", &a, 0, nullptr};        // .a
    MatchContext outerB{"div", "", &b, 0, &top};        // .b (child of .a) ✓
    MatchContext plain{"div", "", nullptr, 0, &outerB}; // -
    MatchContext innerB{"div", "", &b, 0, &plain};      // .b (parent NOT .a)
    MatchContext target{"div", "", &c, 0, &innerB};     // .c

    REQUIRE(resolve({&sheet}, target).opacity == Approx(0.3f));

    // And still no match when no .b has an .a parent.
    MatchContext topPlain{"div", "", nullptr, 0, nullptr};
    MatchContext onlyB{"div", "", &b, 0, &topPlain};
    MatchContext target2{"div", "", &c, 0, &onlyB};
    REQUIRE(resolve({&sheet}, target2).opacity == Approx(1.0f));
}

TEST_CASE("pseudo-classes require the state bit", "[css][cascade]") {
    auto sheet = sheetOf("button:hover { opacity: 0.5; }");
    MatchContext plain{"button", "", nullptr, 0, nullptr};
    REQUIRE(resolve({&sheet}, plain).opacity == Approx(1.0f));
    MatchContext hovered{"button", "", nullptr, kPseudoHover, nullptr};
    REQUIRE(resolve({&sheet}, hovered).opacity == Approx(0.5f));
}

TEST_CASE("later stylesheets win equal specificity", "[css][cascade]") {
    auto first = sheetOf("div { opacity: 0.3; }");
    auto second = sheetOf("div { opacity: 0.7; }");
    MatchContext div{"div", "", nullptr, 0, nullptr};
    REQUIRE(resolve({&first, &second}, div).opacity == Approx(0.7f));
}

TEST_CASE("inline style wins over everything", "[css][cascade]") {
    auto sheet = sheetOf("#x { opacity: 0.9; }");
    MatchContext div{"div", "x", nullptr, 0, nullptr};
    ui::Style inlineStyle;
    inlineStyle.opacity(0.25f);
    ComputedStyle style;
    resolveStyle({&sheet}, div, &inlineStyle.declarations(), &style);
    REQUIRE(style.opacity == Approx(0.25f));
}

TEST_CASE("em resolves against font-size, font-size em against inherited", "[css][cascade]") {
    auto sheet = sheetOf("div { font-size: 20px; width: 2em; }");
    MatchContext div{"div", "", nullptr, 0, nullptr};
    ComputedStyle style;
    style.fontSize = 10.0f; // inherited
    resolveStyle({&sheet}, div, nullptr, &style);
    REQUIRE(style.fontSize == Approx(20.0f));
    REQUIRE(style.width.unit == ui::Unit::Px);
    REQUIRE(style.width.value == Approx(40.0f)); // 2em of the NEW font size

    auto emSheet = sheetOf("div { font-size: 1.5em; }");
    ComputedStyle emStyle;
    emStyle.fontSize = 10.0f;
    resolveStyle({&emSheet}, div, nullptr, &emStyle);
    REQUIRE(emStyle.fontSize == Approx(15.0f));
}
