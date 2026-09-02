#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "css/cascade.hpp"
#include "css/parser.hpp"
#include "ui/transitions.hpp"

#include <fmt/core.h>

using Catch::Approx;
using namespace rendy;
using namespace rendy::css;
using ui::Prop;
using ui::Timing;
using ui::TransitionSpec;

namespace {

std::vector<TransitionSpec> parseTransition(std::string_view value) {
    auto result = parse(fmt::format("x {{ transition: {}; }}", value));
    REQUIRE(result.hasValue());
    for (const Rule& rule : result.value().rules)
        for (const auto& d : rule.declarations)
            if (d.prop == Prop::Transition) return d.value.transitions;
    FAIL("no transition declaration parsed");
    return {};
}

} // namespace

TEST_CASE("parses a single transition", "[css][transition]") {
    const auto specs = parseTransition("background-color 0.25s ease-in-out");
    REQUIRE(specs.size() == 1);
    REQUIRE(specs[0].prop == Prop::BackgroundColor);
    REQUIRE(specs[0].duration == Approx(0.25f));
    REQUIRE(specs[0].delay == 0.0f);
    REQUIRE(specs[0].timing == Timing::EaseInOut);
}

TEST_CASE("parses a transition list with ms, delay and defaults", "[css][transition]") {
    const auto specs = parseTransition("opacity 150ms linear 50ms, width 0.3s");
    REQUIRE(specs.size() == 2);
    REQUIRE(specs[0].prop == Prop::Opacity);
    REQUIRE(specs[0].duration == Approx(0.15f));
    REQUIRE(specs[0].delay == Approx(0.05f));
    REQUIRE(specs[0].timing == Timing::Linear);
    REQUIRE(specs[1].prop == Prop::Width);
    REQUIRE(specs[1].duration == Approx(0.3f));
    REQUIRE(specs[1].timing == Timing::Ease); // default

    // CSS allows a negative delay (starts partway through) but not a
    // negative duration.
    const auto negDelay = parseTransition("opacity 0.2s -50ms");
    REQUIRE(negDelay.size() == 1);
    REQUIRE(negDelay[0].delay == Approx(-0.05f));
}

TEST_CASE("transition 'all' and 'none'", "[css][transition]") {
    const auto all = parseTransition("all 0.2s");
    REQUIRE(all.size() == 1);
    REQUIRE(all[0].prop == Prop::Count); // sentinel for "all"

    // "none" is an empty (but present) declaration that clears transitions.
    auto result = parse("x { transition: none; }");
    REQUIRE(result.hasValue());
    bool found = false;
    for (const Rule& rule : result.value().rules)
        for (const auto& d : rule.declarations)
            if (d.prop == Prop::Transition) {
                found = true;
                REQUIRE(d.value.transitions.empty());
            }
    REQUIRE(found);
}

TEST_CASE("invalid transitions are rejected", "[css][transition]") {
    // Unknown property, missing duration, a third time value and a negative
    // duration all fall to "unsupported".
    for (const char* bad : {"flex-direction 0.2s", "background-color", "0.2s 0.3s 0.4s",
                            "opacity -0.2s"}) {
        auto result = parse(fmt::format("x {{ transition: {}; }}", bad));
        REQUIRE(result.hasValue());
        for (const Rule& rule : result.value().rules)
            for (const auto& d : rule.declarations) REQUIRE(d.prop != Prop::Transition);
    }
}

TEST_CASE("timing functions hit their endpoints and are monotonic-ish",
          "[css][transition]") {
    for (Timing timing : {Timing::Linear, Timing::Ease, Timing::EaseIn, Timing::EaseOut,
                          Timing::EaseInOut}) {
        REQUIRE(ui::anim::ease(timing, 0.0f) == Approx(0.0f).margin(1e-4));
        REQUIRE(ui::anim::ease(timing, 1.0f) == Approx(1.0f).margin(1e-4));
        float prev = -0.01f;
        for (int i = 0; i <= 20; ++i) {
            const float value = ui::anim::ease(timing, static_cast<float>(i) / 20.0f);
            REQUIRE(value >= prev - 1e-3f); // presets are monotonic
            prev = value;
        }
    }
    // Spot-check ease-in-out midpoint symmetry.
    REQUIRE(ui::anim::ease(Timing::EaseInOut, 0.5f) == Approx(0.5f).margin(0.01));
    // ease-in starts slow, ease-out starts fast.
    REQUIRE(ui::anim::ease(Timing::EaseIn, 0.25f) < 0.15f);
    REQUIRE(ui::anim::ease(Timing::EaseOut, 0.25f) > 0.35f);
}

TEST_CASE("expandSpec expands all and border-radius", "[css][transition]") {
    std::vector<TransitionSpec> out;
    ui::anim::expandSpec({Prop::Count, 0.2f, 0.0f, Timing::Ease}, &out);
    REQUIRE(out.size() == 15);
    out.clear();
    ui::anim::expandSpec({Prop::ShadowBlur, 0.2f, 0.0f, Timing::Ease}, &out);
    REQUIRE(out.size() == 4); // box-shadow: offsets + blur + color
    out.clear();
    ui::anim::expandSpec({Prop::BorderRadiusTL, 0.2f, 0.0f, Timing::Ease}, &out);
    REQUIRE(out.size() == 4);
    out.clear();
    ui::anim::expandSpec({Prop::Opacity, 0.2f, 0.0f, Timing::Ease}, &out);
    REQUIRE(out.size() == 1);
}

TEST_CASE("animatable get/set roundtrip", "[css][transition]") {
    ComputedStyle style;
    Vec4 value;
    REQUIRE(ui::anim::getAnimatable(style, Prop::Opacity, &value));
    REQUIRE(value.x == Approx(1.0f));
    ui::anim::setAnimatable(&style, Prop::BackgroundColor, {0.5f, 0.25f, 0.125f, 1.0f});
    REQUIRE(ui::anim::getAnimatable(style, Prop::BackgroundColor, &value));
    REQUIRE(value.y == Approx(0.25f));
    // auto width is not animatable; px width is.
    REQUIRE_FALSE(ui::anim::getAnimatable(style, Prop::Width, &value));
    style.width = ui::Length::px(120.0f);
    REQUIRE(ui::anim::getAnimatable(style, Prop::Width, &value));
    REQUIRE(value.x == Approx(120.0f));
}

TEST_CASE("box-shadow parses and none disables", "[css][shadow]") {
    auto sheet = parse("x { box-shadow: 2 4 10 rgba(0, 0, 0, 0.5); } y { box-shadow: none; }");
    REQUIRE(sheet.hasValue());
    ComputedStyle styleX;
    for (const auto& d : sheet.value().rules[0].declarations) applyDeclaration(d, &styleX);
    REQUIRE(styleX.shadowOffset.x == Approx(2.0f));
    REQUIRE(styleX.shadowOffset.y == Approx(4.0f));
    REQUIRE(styleX.shadowBlur == Approx(10.0f));
    REQUIRE(styleX.shadowColor.a == Approx(0.5f));
    ComputedStyle styleY;
    styleY.shadowBlur = 8.0f;
    for (const auto& d : sheet.value().rules[1].declarations) applyDeclaration(d, &styleY);
    REQUIRE(styleY.shadowBlur == 0.0f);
}
