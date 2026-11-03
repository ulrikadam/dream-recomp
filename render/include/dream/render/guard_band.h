// Cutting polygons down to a range a GPU can rasterise exactly (WP2.3).
//
// The PVR rasterises in screen space and never clips: a title may send vertices it clamped near
// the camera (1/w = 100 or 500) whose screen position is tens of millions of pixels away. A GPU
// handed those loses precision setting the triangle up, and drops or tears it, differently at
// each resolution: Rush 2049's and 18 Wheeler's skies came out partly black.
//
// A polygon with a vertex outside the square [lo, hi] on either axis is replaced, in place, by its
// triangles cut to that square, interpolated as the PVR does in screen space: x, y and 1/w
// linearly, colours and texture coordinates linearly in proportion to 1/w. Polygons inside it,
// and any with a vertex whose 1/w is not positive, are left alone.
//
// Device-independent like display_list.h, so it is unit-tested on every runner.
#pragma once

#include <cstddef>

#include "dream/render/display_list.h"

namespace dream::render {

// Returns the number of polygons replaced.
std::size_t clip_to_guard_band(Frame& frame, float lo = -2048.0f, float hi = 4096.0f);

}  // namespace dream::render
