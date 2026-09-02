#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "css/parser.hpp"

using Catch::Approx;
using namespace rendy;
using namespace rendy::css;
using ui::Length;
using ui::Prop;
using ui::Unit;

namespace {

Stylesheet parseOk(std::string_view text) {
    auto result = parse(text);
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

const ui::Value* find(const Rule& rule, Prop prop) {
    for (const auto& d : rule.declarations)
        if (d.prop == prop) return &d.value;
    return nullptr;
}

} // namespace

TEST_CASE("parses selectors with classes, ids and pseudo", "[css][parser]") {
    auto sheet = parseOk("button.primary:hover { color: red; } #main > .item { width: 10px; }");
    REQUIRE(sheet.rules.size() == 2);

    const auto& first = sheet.rules[0].selectors[0];
    REQUIRE(first.compounds.size() == 1);
    REQUIRE(first.compounds[0].tag == "button");
    REQUIRE(first.compounds[0].classes == std::vector<std::string>{"primary"});
    REQUIRE(first.compounds[0].pseudo == kPseudoHover);

    const auto& second = sheet.rules[1].selectors[0];
    REQUIRE(second.compounds.size() == 2);
    REQUIRE(second.compounds[0].id == "main");
    REQUIRE(second.combinators[0] == Combinator::Child);
}

TEST_CASE("selector lists share declarations", "[css][parser]") {
    auto sheet = parseOk("h1, h2, .big { font-size: 20px; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(sheet.rules[0].selectors.size() == 3);
}

TEST_CASE("padding shorthand expands 1-4 values", "[css][parser]") {
    auto sheet = parseOk("a { padding: 1px 2px 3px 4px; } b { padding: 5px 10px; } c { padding: 7px; }");
    const auto& a = sheet.rules[0];
    REQUIRE(find(a, Prop::PaddingTop)->length == Length::px(1.0f));
    REQUIRE(find(a, Prop::PaddingRight)->length == Length::px(2.0f));
    REQUIRE(find(a, Prop::PaddingBottom)->length == Length::px(3.0f));
    REQUIRE(find(a, Prop::PaddingLeft)->length == Length::px(4.0f));
    const auto& b = sheet.rules[1];
    REQUIRE(find(b, Prop::PaddingTop)->length == Length::px(5.0f));
    REQUIRE(find(b, Prop::PaddingLeft)->length == Length::px(10.0f));
    const auto& c = sheet.rules[2];
    REQUIRE(find(c, Prop::PaddingBottom)->length == Length::px(7.0f));
}

TEST_CASE("flex shorthand", "[css][parser]") {
    auto sheet = parseOk("a { flex: 1; } b { flex: 2 0 100px; } c { flex: none; }");
    REQUIRE(find(sheet.rules[0], Prop::FlexGrow)->number == 1.0f);
    REQUIRE(find(sheet.rules[0], Prop::FlexShrink)->number == 1.0f);
    REQUIRE(find(sheet.rules[1], Prop::FlexGrow)->number == 2.0f);
    REQUIRE(find(sheet.rules[1], Prop::FlexShrink)->number == 0.0f);
    REQUIRE(find(sheet.rules[1], Prop::FlexBasis)->length == Length::px(100.0f));
    REQUIRE(find(sheet.rules[2], Prop::FlexGrow)->number == 0.0f);
}

TEST_CASE("colors in every supported form", "[css][parser]") {
    Color c;
    REQUIRE(parseColorText("#ff8000", &c));
    REQUIRE(c.r == Approx(1.0f));
    REQUIRE(c.g == Approx(128.0f / 255.0f));
    REQUIRE(parseColorText("#f80", &c));
    REQUIRE(c.r == Approx(1.0f));
    REQUIRE(c.b == Approx(0.0f));
    REQUIRE(parseColorText("#11223344", &c));
    REQUIRE(c.a == Approx(0x44 / 255.0f));
    REQUIRE(parseColorText("rgb(255, 0, 0)", &c));
    REQUIRE(c.r == Approx(1.0f));
    REQUIRE(parseColorText("rgba(0, 0, 255, 0.5)", &c));
    REQUIRE(c.b == Approx(1.0f));
    REQUIRE(c.a == Approx(0.5f));
    REQUIRE(parseColorText("teal", &c));
    REQUIRE(c.g == Approx(0x80 / 255.0f));
    REQUIRE(parseColorText("transparent", &c));
    REQUIRE(c.a == Approx(0.0f));
    REQUIRE_FALSE(parseColorText("notacolor", &c));
}

TEST_CASE("unknown properties are collected, parse continues", "[css][parser]") {
    auto sheet = parseOk("a { cursor: pointer; width: 5px; }");
    REQUIRE(sheet.unsupported == std::vector<std::string>{"cursor"});
    REQUIRE(find(sheet.rules[0], Prop::Width) != nullptr);
}

TEST_CASE("malformed rules are skipped without losing later rules", "[css][parser]") {
    auto sheet = parseOk("@media (min-width: 100px) { a { color: red; } } b { width: 1px; }");
    // The @media block fails selector parsing and is skipped whole.
    REQUIRE(!sheet.rules.empty());
    const Rule& last = sheet.rules.back();
    REQUIRE(find(last, Prop::Width) != nullptr);
}

TEST_CASE("gap and border-radius shorthands", "[css][parser]") {
    auto sheet = parseOk("a { gap: 4px 8px; border-radius: 1px 2px 3px 4px; }");
    REQUIRE(find(sheet.rules[0], Prop::RowGap)->number == 4.0f);
    REQUIRE(find(sheet.rules[0], Prop::ColumnGap)->number == 8.0f);
    REQUIRE(find(sheet.rules[0], Prop::BorderRadiusTL)->number == 1.0f);
    REQUIRE(find(sheet.rules[0], Prop::BorderRadiusBL)->number == 4.0f);
}

TEST_CASE("percent and em units", "[css][parser]") {
    auto sheet = parseOk("a { width: 50%; height: 2em; }");
    REQUIRE(find(sheet.rules[0], Prop::Width)->length == Length::percent(50.0f));
    REQUIRE(find(sheet.rules[0], Prop::Height)->length == Length::em(2.0f));
}
