// End-to-end: CSS text → cascade → Yoga → layout rectangles. No GPU needed.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "css/cascade.hpp"
#include "css/parser.hpp"
#include "ui/yoga_layout.hpp"

#include <yoga/Yoga.h>

using Catch::Approx;
using namespace rendy;
using namespace rendy::css;

namespace {

struct TestTree {
    YGConfigRef config;
    std::vector<YGNodeRef> nodes;

    TestTree() {
        config = YGConfigNew();
        YGConfigSetUseWebDefaults(config, true);
    }
    ~TestTree() {
        if (!nodes.empty()) YGNodeFreeRecursive(nodes.front());
        YGConfigFree(config);
    }

    YGNodeRef add(YGNodeRef parent, const ComputedStyle& style) {
        YGNodeRef node = YGNodeNewWithConfig(config);
        ui::applyStyleToYoga(style, node);
        if (parent != nullptr)
            YGNodeInsertChild(parent, node, YGNodeGetChildCount(parent));
        nodes.push_back(node);
        return node;
    }
};

ComputedStyle styled(std::string_view declarations) {
    auto sheet = parse(std::string("x { ") + std::string(declarations) + " }");
    REQUIRE(sheet.hasValue());
    ComputedStyle style;
    MatchContext ctx{"x", "", nullptr, 0, nullptr};
    const Stylesheet& s = sheet.value();
    resolveStyle({&s}, ctx, nullptr, &style);
    return style;
}

} // namespace

TEST_CASE("row layout with gap and padding", "[layout]") {
    TestTree tree;
    YGNodeRef root = tree.add(
        nullptr, styled("flex-direction: row; gap: 10px; padding: 20px; width: 300px; height: 100px;"));
    YGNodeRef a = tree.add(root, styled("width: 50px;"));
    YGNodeRef b = tree.add(root, styled("width: 70px;"));

    YGNodeCalculateLayout(root, YGUndefined, YGUndefined, YGDirectionLTR);

    REQUIRE(YGNodeLayoutGetLeft(a) == Approx(20.0f));
    REQUIRE(YGNodeLayoutGetTop(a) == Approx(20.0f));
    REQUIRE(YGNodeLayoutGetWidth(a) == Approx(50.0f));
    REQUIRE(YGNodeLayoutGetHeight(a) == Approx(60.0f)); // stretched into padding box
    REQUIRE(YGNodeLayoutGetLeft(b) == Approx(20.0f + 50.0f + 10.0f));
}

TEST_CASE("flex-grow distributes remaining space", "[layout]") {
    TestTree tree;
    YGNodeRef root =
        tree.add(nullptr, styled("flex-direction: row; width: 300px; height: 50px;"));
    YGNodeRef a = tree.add(root, styled("flex-grow: 1;"));
    YGNodeRef b = tree.add(root, styled("flex-grow: 2;"));

    YGNodeCalculateLayout(root, YGUndefined, YGUndefined, YGDirectionLTR);

    REQUIRE(YGNodeLayoutGetWidth(a) == Approx(100.0f));
    REQUIRE(YGNodeLayoutGetWidth(b) == Approx(200.0f));
}

TEST_CASE("justify-content center and percent sizes", "[layout]") {
    TestTree tree;
    YGNodeRef root = tree.add(
        nullptr,
        styled("flex-direction: row; justify-content: center; width: 200px; height: 100px;"));
    YGNodeRef child = tree.add(root, styled("width: 50%; height: 50%;"));

    YGNodeCalculateLayout(root, YGUndefined, YGUndefined, YGDirectionLTR);

    REQUIRE(YGNodeLayoutGetWidth(child) == Approx(100.0f));
    REQUIRE(YGNodeLayoutGetHeight(child) == Approx(50.0f));
    REQUIRE(YGNodeLayoutGetLeft(child) == Approx(50.0f));
}

TEST_CASE("absolute positioning with inset", "[layout]") {
    TestTree tree;
    YGNodeRef root = tree.add(nullptr, styled("width: 100px; height: 100px;"));
    YGNodeRef child = tree.add(
        root, styled("position: absolute; right: 10px; bottom: 10px; width: 20px; height: 20px;"));

    YGNodeCalculateLayout(root, YGUndefined, YGUndefined, YGDirectionLTR);

    REQUIRE(YGNodeLayoutGetLeft(child) == Approx(70.0f));
    REQUIRE(YGNodeLayoutGetTop(child) == Approx(70.0f));
}

TEST_CASE("margin auto centers", "[layout]") {
    TestTree tree;
    YGNodeRef root = tree.add(nullptr, styled("width: 200px; height: 100px;"));
    YGNodeRef child = tree.add(root, styled("width: 50px; margin-left: auto; margin-right: auto;"));

    YGNodeCalculateLayout(root, YGUndefined, YGUndefined, YGDirectionLTR);

    REQUIRE(YGNodeLayoutGetLeft(child) == Approx(75.0f));
}

TEST_CASE("min/max clamp sizes", "[layout]") {
    TestTree tree;
    YGNodeRef root = tree.add(nullptr, styled("flex-direction: row; width: 300px; height: 50px;"));
    YGNodeRef a = tree.add(root, styled("flex-grow: 1; max-width: 80px;"));
    YGNodeRef b = tree.add(root, styled("width: 10px; min-width: 40px;"));

    YGNodeCalculateLayout(root, YGUndefined, YGUndefined, YGDirectionLTR);

    REQUIRE(YGNodeLayoutGetWidth(a) == Approx(80.0f));
    REQUIRE(YGNodeLayoutGetWidth(b) == Approx(40.0f));
}
