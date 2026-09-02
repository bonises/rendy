#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "css/parser.hpp"
#include "ui/animations.hpp"

#include <fmt/core.h>

#include <cmath>
#include <limits>

using Catch::Approx;
using namespace rendy;
using namespace rendy::css;
using ui::AnimationSpec;
using ui::AnimDirection;
using ui::Prop;
using ui::Timing;

namespace {

std::vector<AnimationSpec> parseAnimation(std::string_view value) {
    auto result = parse(fmt::format("x {{ animation: {}; }}", value));
    REQUIRE(result.hasValue());
    for (const Rule& rule : result.value().rules)
        for (const auto& d : rule.declarations)
            if (d.prop == Prop::Animation) return d.value.animations;
    FAIL("no animation declaration parsed");
    return {};
}

} // namespace

TEST_CASE("@keyframes parses offsets, from/to and merges duplicates", "[css][animation]") {
    auto result = parse(R"(
        @keyframes pulse {
            from { opacity: 1; }
            50%  { opacity: 0.4; }
            to   { opacity: 1; }
        }
        @keyframes slide {
            0%, 100% { width: 10px; }
            50%      { width: 30px; }
            50%      { opacity: 0.5; }
        }
    )");
    REQUIRE(result.hasValue());
    const Stylesheet& sheet = result.value();
    REQUIRE(sheet.keyframes.size() == 2);

    const KeyframesRule& pulse = sheet.keyframes[0];
    REQUIRE(pulse.name == "pulse");
    REQUIRE(pulse.frames.size() == 3);
    REQUIRE(pulse.frames[0].offset == Approx(0.0f));
    REQUIRE(pulse.frames[1].offset == Approx(0.5f));
    REQUIRE(pulse.frames[2].offset == Approx(1.0f));

    // Two 50% blocks merged into one frame with both declarations.
    const KeyframesRule& slide = sheet.keyframes[1];
    REQUIRE(slide.frames.size() == 3);
    REQUIRE(slide.frames[1].offset == Approx(0.5f));
    REQUIRE(slide.frames[1].declarations.size() == 2);
}

TEST_CASE("unknown at-rules are skipped without eating the sheet", "[css][animation]") {
    auto result = parse(R"(
        @import "other.css";
        @media (max-width: 100px) { div { opacity: 0; } }
        button { opacity: 0.5; }
    )");
    REQUIRE(result.hasValue());
    REQUIRE(result.value().rules.size() == 1); // the button rule survives
    REQUIRE(result.value().unsupported.size() >= 2);
}

TEST_CASE("animation shorthand parses", "[css][animation]") {
    const auto full = parseAnimation("pulse 1.2s ease-in-out 100ms infinite alternate");
    REQUIRE(full.size() == 1);
    REQUIRE(full[0].name == "pulse");
    REQUIRE(full[0].duration == Approx(1.2f));
    REQUIRE(full[0].delay == Approx(0.1f));
    REQUIRE(full[0].timing == Timing::EaseInOut);
    REQUIRE(std::isinf(full[0].iterations));
    REQUIRE(full[0].direction == AnimDirection::Alternate);
    REQUIRE_FALSE(full[0].fillForwards);

    const auto minimal = parseAnimation("intro 300ms 1 forwards");
    REQUIRE(minimal.size() == 1);
    REQUIRE(minimal[0].iterations == Approx(1.0f));
    REQUIRE(minimal[0].fillForwards);

    const auto list = parseAnimation("a 1s, b 2s reverse");
    REQUIRE(list.size() == 2);
    REQUIRE(list[1].direction == AnimDirection::Reverse);

    // none clears.
    REQUIRE(parseAnimation("none").empty());
}

TEST_CASE("invalid animations are rejected", "[css][animation]") {
    // Missing name, missing duration, negative duration, two names.
    for (const char* bad : {"1s", "pulse", "pulse -1s", "pulse extra 1s"}) {
        auto result = parse(fmt::format("x {{ animation: {}; }}", bad));
        REQUIRE(result.hasValue());
        for (const Rule& rule : result.value().rules)
            for (const auto& d : rule.declarations) REQUIRE(d.prop != Prop::Animation);
    }
}

TEST_CASE("timeline: delay, iterations, direction, fill", "[css][animation]") {
    AnimationSpec spec;
    spec.duration = 1.0f;
    spec.delay = 0.5f;
    spec.iterations = 2.0f;
    spec.direction = AnimDirection::Alternate;

    // In the delay: inactive.
    REQUIRE_FALSE(ui::anim::sampleTimeline(spec, 0.25f).active);
    // First iteration runs forward.
    auto s = ui::anim::sampleTimeline(spec, 0.5f + 0.25f);
    REQUIRE(s.active);
    REQUIRE(s.progress == Approx(0.25f));
    // Second iteration alternates (runs backward).
    s = ui::anim::sampleTimeline(spec, 0.5f + 1.25f);
    REQUIRE(s.progress == Approx(0.75f));
    // Past the end without fill: finished, nothing applied.
    s = ui::anim::sampleTimeline(spec, 5.0f);
    REQUIRE(s.finished);
    REQUIRE_FALSE(s.active);
    // With fill forwards the end pose holds — 2 alternate iterations end at 0.
    spec.fillForwards = true;
    s = ui::anim::sampleTimeline(spec, 5.0f);
    REQUIRE(s.active);
    REQUIRE(s.progress == Approx(0.0f));

    // Infinite never finishes.
    spec.iterations = std::numeric_limits<float>::infinity();
    s = ui::anim::sampleTimeline(spec, 1000.25f);
    REQUIRE(s.active);
    REQUIRE_FALSE(s.finished);

    // Reverse plays backward from the start.
    AnimationSpec rev;
    rev.duration = 1.0f;
    rev.direction = AnimDirection::Reverse;
    REQUIRE(ui::anim::sampleTimeline(rev, 0.25f).progress == Approx(0.75f));

    // Fractional iteration count ends mid-timeline.
    AnimationSpec half;
    half.duration = 1.0f;
    half.iterations = 1.5f;
    half.fillForwards = true;
    s = ui::anim::sampleTimeline(half, 9.0f);
    REQUIRE(s.finished);
    REQUIRE(s.progress == Approx(0.5f));
}

TEST_CASE("tracks compile against the base style and interpolate", "[css][animation]") {
    auto result = parse(R"(
        @keyframes fade { 50% { opacity: 0.0; } }
    )");
    REQUIRE(result.hasValue());
    const auto& frames = result.value().keyframes[0].frames;

    ComputedStyle base;
    base.opacity = 1.0f;
    const auto tracks = ui::anim::compileTracks(frames, base);
    REQUIRE(tracks.size() == 1);
    REQUIRE(tracks[0].prop == Prop::Opacity);
    // Implicit 0% and 100% from the base value.
    REQUIRE(tracks[0].keys.size() == 3);
    REQUIRE(tracks[0].keys.front().value.x == Approx(1.0f));
    REQUIRE(tracks[0].keys.back().value.x == Approx(1.0f));

    // Linear sampling: halfway into the first segment.
    const Vec4 mid = ui::anim::sampleTrack(tracks[0], 0.25f, Timing::Linear);
    REQUIRE(mid.x == Approx(0.5f));
    // Endpoints exact.
    REQUIRE(ui::anim::sampleTrack(tracks[0], 0.0f, Timing::Linear).x == Approx(1.0f));
    REQUIRE(ui::anim::sampleTrack(tracks[0], 0.5f, Timing::Linear).x == Approx(0.0f));
    REQUIRE(ui::anim::sampleTrack(tracks[0], 1.0f, Timing::Linear).x == Approx(1.0f));
}

TEST_CASE("per-keyframe timing functions parse and ease their own segment",
          "[css][animation]") {
    auto result = parse(R"(
        @keyframes fade {
            from { opacity: 1; animation-timing-function: ease-in; }
            50%  { opacity: 0; }
            to   { opacity: 1; }
        }
    )");
    REQUIRE(result.hasValue());
    const auto& frames = result.value().keyframes[0].frames;
    REQUIRE(frames.size() == 3);
    REQUIRE(frames[0].hasTiming);
    REQUIRE(frames[0].timing == Timing::EaseIn);
    REQUIRE_FALSE(frames[1].hasTiming);
    // The timing declaration is metadata, not an animated property.
    REQUIRE(frames[0].declarations.size() == 1);

    ComputedStyle base;
    base.opacity = 1.0f;
    const auto tracks = ui::anim::compileTracks(frames, base);
    REQUIRE(tracks.size() == 1);

    // First segment (0 → 0.5) eases in: halfway through, ease-in is well
    // below linear. Second segment falls back to the animation's timing
    // (linear here) and interpolates exactly.
    const float easedIn = 1.0f - ui::anim::sampleTrack(tracks[0], 0.25f, Timing::Linear).x;
    REQUIRE(easedIn < 0.35f);
    REQUIRE(ui::anim::sampleTrack(tracks[0], 0.75f, Timing::Linear).x == Approx(0.5f));
}

TEST_CASE("animation-timing-function on a rule overrides animation timings",
          "[css][animation]") {
    auto result = parse(R"(
        x { animation: pulse 1s ease; animation-timing-function: linear; }
    )");
    REQUIRE(result.hasValue());
    ComputedStyle style;
    for (const auto& d : result.value().rules[0].declarations) applyDeclaration(d, &style);
    REQUIRE(style.animations.size() == 1);
    REQUIRE(style.animations[0].timing == Timing::Linear);

    // Invalid values are rejected (unknown ident, multiple tokens).
    for (const char* bad : {"bouncy", "ease linear"}) {
        auto r = parse(fmt::format("x {{ animation-timing-function: {}; }}", bad));
        REQUIRE(r.hasValue());
        REQUIRE_FALSE(r.value().unsupported.empty());
    }
}

TEST_CASE("track for an unanimatable base value is dropped", "[css][animation]") {
    auto result = parse(R"(
        @keyframes grow { 50% { width: 100px; } }
    )");
    REQUIRE(result.hasValue());
    ComputedStyle base; // width defaults to auto → can't animate from it
    const auto tracks = ui::anim::compileTracks(result.value().keyframes[0].frames, base);
    REQUIRE(tracks.empty());
}
