#include "dream/render/guard_band.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace dream::render {

namespace {

// A vertex with every attribute that varies with perspective already multiplied by 1/w, so all of
// them interpolate linearly in screen space.
struct ClipVertex {
    float x, y, z;
    float u, v;
    float base[4], offset[4];
};

ClipVertex lift(const Vertex& v) {
    ClipVertex c{v.x, v.y, v.z, v.u * v.z, v.v * v.z, {}, {}};
    for (int i = 0; i < 4; ++i) {
        c.base[i] = static_cast<float>((v.base >> (8 * i)) & 0xFFu) * v.z;
        c.offset[i] = static_cast<float>((v.offset >> (8 * i)) & 0xFFu) * v.z;
    }
    return c;
}

std::uint32_t pack(const float* channels, float z) {
    std::uint32_t out = 0;
    for (int i = 0; i < 4; ++i) {
        const float c = std::clamp(std::round(channels[i] / z), 0.0f, 255.0f);
        out |= static_cast<std::uint32_t>(c) << (8 * i);
    }
    return out;
}

Vertex lower(const ClipVertex& c) {
    Vertex v;
    v.x = c.x;
    v.y = c.y;
    v.z = c.z;
    v.u = c.u / c.z;
    v.v = c.v / c.z;
    v.base = pack(c.base, c.z);
    v.offset = pack(c.offset, c.z);
    return v;
}

ClipVertex lerp(const ClipVertex& a, const ClipVertex& b, float t) {
    auto mix = [t](float p, float q) { return p + (q - p) * t; };
    ClipVertex c{mix(a.x, b.x), mix(a.y, b.y), mix(a.z, b.z), mix(a.u, b.u), mix(a.v, b.v), {}, {}};
    for (int i = 0; i < 4; ++i) {
        c.base[i] = mix(a.base[i], b.base[i]);
        c.offset[i] = mix(a.offset[i], b.offset[i]);
    }
    return c;
}

// One Sutherland-Hodgman pass: keep the part of `in` where `inside` holds, cutting each edge that
// crosses at `edge` (the coordinate the crossing has on the clipped axis).
template <typename Inside, typename Edge>
std::vector<ClipVertex> cut(const std::vector<ClipVertex>& in, Inside inside, Edge edge) {
    std::vector<ClipVertex> out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const ClipVertex& a = in[i];
        const ClipVertex& b = in[(i + 1) % in.size()];
        const bool ia = inside(a), ib = inside(b);
        if (ia)
            out.push_back(a);
        if (ia != ib)
            out.push_back(lerp(a, b, edge(a, b)));
    }
    return out;
}

std::vector<ClipVertex> clip(std::vector<ClipVertex> p, float lo, float hi) {
    auto t_at = [](float pa, float pb, float bound) { return (bound - pa) / (pb - pa); };
    p = cut(
        p, [lo](const ClipVertex& c) { return c.x >= lo; },
        [&](const ClipVertex& a, const ClipVertex& b) { return t_at(a.x, b.x, lo); });
    p = cut(
        p, [hi](const ClipVertex& c) { return c.x <= hi; },
        [&](const ClipVertex& a, const ClipVertex& b) { return t_at(a.x, b.x, hi); });
    p = cut(
        p, [lo](const ClipVertex& c) { return c.y >= lo; },
        [&](const ClipVertex& a, const ClipVertex& b) { return t_at(a.y, b.y, lo); });
    p = cut(
        p, [hi](const ClipVertex& c) { return c.y <= hi; },
        [&](const ClipVertex& a, const ClipVertex& b) { return t_at(a.y, b.y, hi); });
    return p;
}

}  // namespace

std::size_t clip_to_guard_band(Frame& frame, float lo, float hi) {
    std::size_t replaced = 0;
    auto outside = [&](const Vertex& v) { return v.x < lo || v.x > hi || v.y < lo || v.y > hi; };
    for (std::vector<Polygon>& list : frame.lists) {
        std::vector<Polygon> rebuilt;
        rebuilt.reserve(list.size());
        for (const Polygon& p : list) {
            bool needs = false, positive = true;
            for (std::uint32_t i = 0; i < p.count; ++i) {
                const Vertex& v = frame.vertices[p.first + i];
                needs = needs || outside(v);
                positive = positive && v.z > 0.0f;
            }
            if (!needs || !positive || p.count < 3) {
                rebuilt.push_back(p);
                continue;
            }
            ++replaced;
            // Each triangle of the strip in the order the GPU rasterises it (every second one
            // swaps its first two vertices to keep the winding), cut, then fanned back out.
            for (std::uint32_t i = 0; i + 2 < p.count; ++i) {
                std::uint32_t a = p.first + i, b = a + 1;
                if (i & 1u)
                    std::swap(a, b);
                const std::vector<ClipVertex> poly =
                    clip({lift(frame.vertices[a]), lift(frame.vertices[b]),
                          lift(frame.vertices[p.first + i + 2])},
                         lo, hi);
                for (std::size_t k = 1; k + 1 < poly.size(); ++k) {
                    Polygon t = p;
                    t.first = static_cast<std::uint32_t>(frame.vertices.size());
                    t.count = 3;
                    frame.vertices.push_back(lower(poly[0]));
                    frame.vertices.push_back(lower(poly[k]));
                    frame.vertices.push_back(lower(poly[k + 1]));
                    rebuilt.push_back(t);
                }
            }
        }
        list = std::move(rebuilt);
    }
    return replaced;
}

}  // namespace dream::render
