#include <catch2/catch_test_macros.hpp>

#include "canvas/canvas_data.hpp"

#include <cstdint>

using namespace rendy;
using namespace rendy::detail;

namespace {

Quad2D quad(float x, uint32_t clipIndex) {
    Quad2D q{};
    q.rect = {x, 0.0f, 10.0f, 10.0f};
    q.info.z = static_cast<float>(clipIndex);
    return q;
}

} // namespace

TEST_CASE("snapshot captures and replays with rebased clips", "[canvas][damage]") {
    CanvasData data;
    data.reset({800.0f, 600.0f}); // clip 0 = viewport

    // An app-drawn quad before the UI (not part of the snapshot).
    data.quads.push_back(quad(1.0f, 0));

    const size_t quadBase = data.quads.size();
    const size_t clipBase = data.clips.size();

    // "UI paint": one viewport-clipped quad, then two under a pushed clip.
    data.quads.push_back(quad(2.0f, 0));
    data.clips.push_back({10.0f, 10.0f, 100.0f, 100.0f});
    data.quads.push_back(quad(3.0f, 1));
    data.clips.push_back({20.0f, 20.0f, 90.0f, 90.0f});
    data.quads.push_back(quad(4.0f, 2));

    CanvasSnapshot snapshot;
    REQUIRE(captureSnapshot(data, quadBase, clipBase, &snapshot));
    REQUIRE(snapshot.quads.size() == 3);
    REQUIRE(snapshot.clips.size() == 2);
    REQUIRE(snapshot.quads[0].info.z == 0.0f); // viewport clip stays 0
    REQUIRE(snapshot.quads[1].info.z == 1.0f); // rebased
    REQUIRE(snapshot.quads[2].info.z == 2.0f);

    // Next frame: different pre-existing state (an extra app clip first).
    CanvasData next;
    next.reset({800.0f, 600.0f});
    next.clips.push_back({0.0f, 0.0f, 50.0f, 50.0f}); // app clip at index 1
    next.quads.push_back(quad(1.0f, 1));

    replaySnapshot(snapshot, &next);
    REQUIRE(next.quads.size() == 4);
    REQUIRE(next.clips.size() == 4); // viewport + app + 2 replayed
    REQUIRE(next.quads[1].info.z == 0.0f); // viewport-clipped stays 0
    REQUIRE(next.quads[2].info.z == 2.0f); // first replayed clip landed at 2
    REQUIRE(next.quads[3].info.z == 3.0f);
    // The replayed clip rects came through intact.
    REQUIRE(next.clips[2] == Vec4{10.0f, 10.0f, 100.0f, 100.0f});
    REQUIRE(next.clips[3] == Vec4{20.0f, 20.0f, 90.0f, 90.0f});
    // Quad payloads are byte-identical apart from the clip index.
    REQUIRE(next.quads[2].rect.x == 3.0f);
}

TEST_CASE("snapshot refuses foreign clip references", "[canvas][damage]") {
    CanvasData data;
    data.reset({800.0f, 600.0f});
    data.clips.push_back({0.0f, 0.0f, 50.0f, 50.0f}); // pre-existing app clip 1

    const size_t quadBase = data.quads.size();
    const size_t clipBase = data.clips.size();
    data.quads.push_back(quad(1.0f, 1)); // references the app clip

    CanvasSnapshot snapshot;
    REQUIRE_FALSE(captureSnapshot(data, quadBase, clipBase, &snapshot));
    REQUIRE(snapshot.quads.empty());
}
