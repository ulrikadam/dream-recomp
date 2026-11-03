// Polygons with vertices far off screen, cut down before they reach a GPU (guard_band.h).
#include <cmath>

#include "dream/render/guard_band.h"

#include "doctest.h"

using namespace dream::render;

namespace {

Frame one_strip(std::initializer_list<Vertex> vs) {
    Frame f;
    for (const Vertex& v : vs) f.vertices.push_back(v);
    Polygon p;
    p.first = 0;
    p.count = static_cast<std::uint32_t>(vs.size());
    p.pcw = 0x808e010au;
    f.lists[0].push_back(p);
    return f;
}

Vertex vertex(float x, float y, float z, float u, std::uint32_t base) {
    Vertex v;
    v.x = x;
    v.y = y;
    v.z = z;
    v.u = u;
    v.v = 0.0f;
    v.base = base;
    return v;
}

}  // namespace

TEST_CASE("guard band: a polygon on screen is left as it is") {
    Frame f =
        one_strip({vertex(10, 10, 1, 0, 0), vertex(100, 10, 1, 1, 0), vertex(10, 100, 1, 0, 0)});
    CHECK(clip_to_guard_band(f) == 0);
    REQUIRE(f.lists[0].size() == 1);
    CHECK(f.lists[0][0].count == 3);
}

TEST_CASE("guard band: a vertex clamped near the camera is cut back, with perspective") {
    // Rush 2049's sky: two vertices at the horizon, far away, and one clamped at 1/w = 100 that
    // projects tens of millions of pixels up and to the side.
    Frame f = one_strip({vertex(345.4f, 34.5f, 0.0003f, 0.0f, 0xff0000f3u),
                         vertex(555.8f, 9.8f, 0.0004f, 1.0f, 0xffac97ffu),
                         vertex(2532901.8f, -73660624.0f, 100.0f, 0.5f, 0xff000090u)});
    CHECK(clip_to_guard_band(f, -2048.0f, 4096.0f) == 1);
    REQUIRE(!f.lists[0].empty());
    for (const Polygon& p : f.lists[0]) {
        CHECK(p.count == 3);
        CHECK(p.pcw == 0x808e010au);  // the words that say how to draw it come along
        for (std::uint32_t i = 0; i < 3; ++i) {
            const Vertex& v = f.vertices[p.first + i];
            CHECK(v.x >= -2048.0f);
            CHECK(v.x <= 4096.0f);
            CHECK(v.y >= -2048.0f);
            CHECK(v.y <= 4096.0f);
            CHECK(v.z > 0.0f);
        }
    }
    // The horizon vertices survive unchanged.
    bool kept_first = false;
    for (const Polygon& p : f.lists[0])
        for (std::uint32_t i = 0; i < 3; ++i) {
            const Vertex& v = f.vertices[p.first + i];
            if (v.x == 345.4f && v.y == 34.5f)
                kept_first = v.z == 0.0003f && v.base == 0xff0000f3u;
        }
    CHECK(kept_first);
}

TEST_CASE("guard band: a cut point interpolates 1/w linearly and the rest over 1/w") {
    // An edge from (0, 0, z = 1, u = 0) to (8192, 0, z = 3, u = 1), cut at x = 4096 (t = 0.5):
    // 1/w is linear on screen, so z = 2; u is linear in u/w, so u = (0 * 1 + 1 * 3) / 2 / 2 = 0.75.
    Frame f = one_strip({vertex(0, 0, 1, 0, 0), vertex(8192, 0, 3, 1, 0), vertex(0, 100, 1, 0, 0)});
    CHECK(clip_to_guard_band(f, -2048.0f, 4096.0f) == 1);
    bool found = false;
    for (const Polygon& p : f.lists[0])
        for (std::uint32_t i = 0; i < 3; ++i) {
            const Vertex& v = f.vertices[p.first + i];
            if (std::abs(v.x - 4096.0f) < 1e-3f && std::abs(v.y) < 1e-3f) {
                found = true;
                CHECK(v.z == doctest::Approx(2.0f));
                CHECK(v.u == doctest::Approx(0.75f));
            }
        }
    CHECK(found);
}

TEST_CASE("guard band: a vertex behind the camera leaves the polygon alone") {
    Frame f =
        one_strip({vertex(10, 10, 1, 0, 0), vertex(1e8f, 10, -1, 1, 0), vertex(10, 100, 1, 0, 0)});
    CHECK(clip_to_guard_band(f) == 0);
    CHECK(f.lists[0].size() == 1);
}
