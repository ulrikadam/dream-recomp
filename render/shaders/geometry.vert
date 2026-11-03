// Opaque geometry (WP2.3 step 3). The Tile Accelerator's coordinates are already in screen space,
// so there is no model or view transform: x and y are pixels, and z is 1/w (larger is nearer).
//
// The rasteriser is given the real w (the inverse of z) with x and y multiplied by it, as Flycast
// does, so it clips in homogeneous space and interpolates colours and texture coordinates with
// perspective itself. It used to be given w = 1 with the attributes divided by hand, and a vertex
// a title clamps near the camera (1/w = 500, projected thousands of pixels off screen) then tore
// its polygon apart: 18 Wheeler's sky drew black, Daytona USA 2001's grass white or black at the
// screen edge. 1/w is linear in screen space, so it and the guest position go unperspectived, for
// depth, fog and the user clip.
#version 450

layout(push_constant) uniform Push {
    vec2 scale;   // 2 / the region of the guest screen being shown
    vec2 offset;
    int mode;         // used by the fragment shader
    float alpha_ref;  // used by the fragment shader
    vec4 clip;        // used by the fragment shader
    int clip_mode;
} push;

layout(location = 0) in vec3 in_pos;   // x, y in pixels; z is 1/w
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_base;  // ARGB unpacked to RGBA floats by the vertex format
layout(location = 3) in vec4 in_offset;

layout(location = 0) out vec4 v_base;
layout(location = 1) out vec4 v_offset;
layout(location = 2) out vec2 v_uv;
layout(location = 3) noperspective out vec2 v_pos;    // x, y in guest pixels, for the user clip
layout(location = 4) noperspective out float v_inv_w;

void main() {
    const float inv_w = in_pos.z;
    v_base = in_base;
    v_offset = in_offset;
    v_uv = in_uv;
    v_pos = in_pos.xy;
    v_inv_w = inv_w;
    // A non-positive 1/w is not a point in front of the camera; keep w finite and positive so the
    // vertex is clipped rather than wrapping round through infinity.
    const float w = 1.0 / max(inv_w, 1e-12);
    gl_Position = vec4((in_pos.xy * push.scale + push.offset) * w, 0.0, w);
}
