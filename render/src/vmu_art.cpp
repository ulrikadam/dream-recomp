#include "dream/render/vmu_art.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "dream/render/overlay.h"

namespace dream::render {
namespace {

constexpr std::uint32_t rgb(unsigned r, unsigned g, unsigned b) {
    return 0xFF000000u | b << 16 | g << 8 | r;
}

// The colours of the shell, the screen and the buttons.
constexpr std::uint32_t kEdge = rgb(140, 140, 176), kShell = rgb(234, 234, 240),
                        kRail = rgb(220, 220, 231), kWindow = rgb(243, 243, 248),
                        kWindowEdge = rgb(200, 200, 218), kLabel = rgb(106, 106, 160),
                        kBezel = rgb(206, 206, 220), kGlassTop = rgb(158, 217, 192),
                        kGlassBottom = rgb(124, 194, 164), kPixel = rgb(28, 54, 43),
                        kPadDark = rgb(60, 60, 102), kPadFace = rgb(138, 138, 182),
                        kPadCross = rgb(158, 158, 200), kPressed = rgb(92, 92, 138),
                        kInk = rgb(46, 46, 68), kWhite = rgb(255, 255, 255);

// --- the screen's font -----------------------------------------------------------------------

struct Glyph {
    char ch;
    const char* rows[5];
};

constexpr Glyph kLcdFont[] = {
    {'0', {"###", "#.#", "#.#", "#.#", "###"}}, {'1', {".#.", "##.", ".#.", ".#.", "###"}},
    {'2', {"###", "..#", "###", "#..", "###"}}, {'3', {"###", "..#", ".##", "..#", "###"}},
    {'4', {"#.#", "#.#", "###", "..#", "..#"}}, {'5', {"###", "#..", "###", "..#", "###"}},
    {'6', {"###", "#..", "###", "#.#", "###"}}, {'7', {"###", "..#", "..#", ".#.", ".#."}},
    {'8', {"###", "#.#", "###", "#.#", "###"}}, {'9', {"###", "#.#", "###", "..#", "###"}},
    {'A', {".#.", "#.#", "###", "#.#", "#.#"}}, {'B', {"##.", "#.#", "##.", "#.#", "##."}},
    {'C', {".##", "#..", "#..", "#..", ".##"}}, {'D', {"##.", "#.#", "#.#", "#.#", "##."}},
    {'E', {"###", "#..", "##.", "#..", "###"}}, {'F', {"###", "#..", "##.", "#..", "#.."}},
    {'G', {".##", "#..", "#.#", "#.#", ".##"}}, {'H', {"#.#", "#.#", "###", "#.#", "#.#"}},
    {'I', {"###", ".#.", ".#.", ".#.", "###"}}, {'J', {"..#", "..#", "..#", "#.#", ".#."}},
    {'K', {"#.#", "#.#", "##.", "#.#", "#.#"}}, {'L', {"#..", "#..", "#..", "#..", "###"}},
    {'M', {"#.#", "###", "###", "#.#", "#.#"}}, {'N', {"##.", "#.#", "#.#", "#.#", "#.#"}},
    {'O', {".#.", "#.#", "#.#", "#.#", ".#."}}, {'P', {"##.", "#.#", "##.", "#..", "#.."}},
    {'Q', {".#.", "#.#", "#.#", "##.", ".##"}}, {'R', {"##.", "#.#", "##.", "#.#", "#.#"}},
    {'S', {".##", "#..", ".#.", "..#", "##."}}, {'T', {"###", ".#.", ".#.", ".#.", ".#."}},
    {'U', {"#.#", "#.#", "#.#", "#.#", "###"}}, {'V', {"#.#", "#.#", "#.#", "#.#", ".#."}},
    {'W', {"#.#", "#.#", "###", "###", "#.#"}}, {'X', {"#.#", "#.#", ".#.", "#.#", "#.#"}},
    {'Y', {"#.#", "#.#", ".#.", ".#.", ".#."}}, {'Z', {"###", "..#", ".#.", "#..", "###"}},
    {'/', {"..#", "..#", ".#.", "#..", "#.."}}, {'-', {"...", "...", "###", "...", "..."}},
    {'+', {"...", ".#.", "###", ".#.", "..."}}, {':', {"...", ".#.", "...", ".#.", "..."}},
    {'.', {"...", "...", "...", "...", ".#."}}, {' ', {"...", "...", "...", "...", "..."}},
};

const Glyph* lcd_glyph(char c) {
    if (c >= 'a' && c <= 'z')
        c = static_cast<char>(c - 'a' + 'A');
    for (const Glyph& g : kLcdFont)
        if (g.ch == c)
            return &g;
    return nullptr;
}

// --- shapes with soft edges ------------------------------------------------------------------

struct Canvas {
    std::uint32_t* px;
    unsigned pitch, width, height;

    void blend(int x, int y, std::uint32_t c, float a) {
        if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height) ||
            a <= 0.0f)
            return;
        std::uint32_t& d = px[static_cast<std::size_t>(y) * pitch + static_cast<unsigned>(x)];
        if (a >= 1.0f) {
            d = c | 0xFF000000u;
            return;
        }
        std::uint32_t out = 0xFF000000u;
        for (unsigned shift = 0; shift < 24; shift += 8) {
            const float s = static_cast<float>((c >> shift) & 0xFF),
                        t = static_cast<float>((d >> shift) & 0xFF);
            out |= static_cast<std::uint32_t>(std::lround(s * a + t * (1.0f - a))) << shift;
        }
        d = out;
    }

    // Fills where `sdf` (a signed distance in pixels, negative inside) covers the pixel centres in
    // the box, with a one-pixel soft edge; `colour(y)` and `alpha` let a fill shade or show
    // through.
    template <typename Sdf, typename Colour>
    void fill(float x0, float y0, float x1, float y1, Sdf sdf, Colour colour, float alpha = 1.0f) {
        const int ix0 = std::max(0, static_cast<int>(std::floor(x0)) - 1),
                  iy0 = std::max(0, static_cast<int>(std::floor(y0)) - 1),
                  ix1 = std::min(static_cast<int>(width) - 1, static_cast<int>(std::ceil(x1)) + 1),
                  iy1 = std::min(static_cast<int>(height) - 1, static_cast<int>(std::ceil(y1)) + 1);
        for (int y = iy0; y <= iy1; ++y)
            for (int x = ix0; x <= ix1; ++x) {
                const float d = sdf(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
                const float cover = std::clamp(0.5f - d, 0.0f, 1.0f);
                if (cover > 0.0f)
                    blend(x, y, colour(static_cast<float>(y)), cover * alpha);
            }
    }
};

float sd_round_rect(float px, float py, float x0, float y0, float x1, float y1, float r) {
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const float hw = (x1 - x0) * 0.5f, hh = (y1 - y0) * 0.5f;
    r = std::min({r, hw, hh});
    const float qx = std::fabs(px - cx) - hw + r, qy = std::fabs(py - cy) - hh + r;
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

float sd_circle(float px, float py, float cx, float cy, float r) {
    return std::hypot(px - cx, py - cy) - r;
}

void round_rect(Canvas& cv, float x0, float y0, float x1, float y1, float r, std::uint32_t c,
                float alpha = 1.0f) {
    cv.fill(
        x0, y0, x1, y1,
        [&](float px, float py) { return sd_round_rect(px, py, x0, y0, x1, y1, r); },
        [&](float) { return c; }, alpha);
}

void circle(Canvas& cv, float cx, float cy, float r, std::uint32_t c, float alpha = 1.0f) {
    cv.fill(
        cx - r, cy - r, cx + r, cy + r,
        [&](float px, float py) { return sd_circle(px, py, cx, cy, r); }, [&](float) { return c; },
        alpha);
}

// A shape with an outline: the edge colour, then the face inset by `line`.
void outlined_rect(Canvas& cv, float x0, float y0, float x1, float y1, float r, float line,
                   std::uint32_t edge, std::uint32_t face) {
    round_rect(cv, x0, y0, x1, y1, r, edge);
    round_rect(cv, x0 + line, y0 + line, x1 - line, y1 - line, std::max(0.0f, r - line), face);
}

void outlined_circle(Canvas& cv, float cx, float cy, float r, float line, std::uint32_t edge,
                     std::uint32_t face) {
    circle(cv, cx, cy, r, edge);
    circle(cv, cx, cy, r - line, face);
}

std::uint32_t mix(std::uint32_t a, std::uint32_t b, float t) {
    std::uint32_t out = 0xFF000000u;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        const float s = static_cast<float>((a >> shift) & 0xFF),
                    e = static_cast<float>((b >> shift) & 0xFF);
        out |= static_cast<std::uint32_t>(std::lround(s + (e - s) * t)) << shift;
    }
    return out;
}

void centred_text(Canvas& cv, float cx, float top, std::string_view s, unsigned scale,
                  std::uint32_t c) {
    TextStyle st;
    st.fg = c;
    st.bg = 0;
    st.scale = scale;
    const int w = static_cast<int>(text_width(s, scale)) - static_cast<int>(scale);
    draw_text(cv.px, cv.pitch, cv.width, cv.height, static_cast<int>(std::lround(cx)) - w / 2,
              static_cast<int>(std::lround(top)), s, st);
}

bool pressed_at(const VmuPressed& p, VmuButton b) {
    return p[static_cast<unsigned>(b)];
}

}  // namespace

// --- the screen ------------------------------------------------------------------------------

int vmu_lcd_text_width(std::string_view text) {
    return text.empty() ? 0 : static_cast<int>(text.size()) * 4 - 1;
}

int vmu_lcd_text(VmuLcd& lcd, int x, int y, std::string_view text) {
    for (char c : text) {
        if (const Glyph* g = lcd_glyph(c))
            for (int row = 0; row < 5; ++row)
                for (int col = 0; col < 3; ++col)
                    if (g->rows[row][col] == '#')
                        lcd.set(x + col, y + row);
        x += 4;
    }
    return x;
}

VmuLcd vmu_file_screen(const maple::VmuFile* file, unsigned index, unsigned count, bool new_card) {
    VmuLcd lcd;
    auto centre = [&](int y, std::string_view s) {
        vmu_lcd_text(lcd, (VmuLcd::kWidth - vmu_lcd_text_width(s)) / 2, y, s);
    };
    if (new_card) {
        for (int i = -6; i <= 6; ++i) {  // a large plus
            for (int t = -1; t <= 1; ++t) {
                lcd.set(24 + i, 11 + t);
                lcd.set(24 + t, 11 + i);
            }
        }
        centre(24, "CREER");
        return lcd;
    }
    if (!file) {
        centre(13, "VIDE");
        return lcd;
    }
    // The icon in one colour: what is opaque and not light. An icon drawn light on a dark ground
    // would come out as a dark block with its picture as holes, so when most of what is opaque
    // would be lit, it is shown the other way round.
    if (file->icon.size() == maple::kVmuIconSize * maple::kVmuIconSize) {
        VmuLcd icon;
        unsigned opaque = 0, dark = 0;
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) {
                const std::uint32_t c = file->icon[static_cast<std::size_t>(y * 32 + x)];
                const unsigned a = c >> 24, r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
                if (a < 128)
                    continue;
                ++opaque;
                if (r * 3 + g * 6 + b < 10 * 190) {
                    icon.set(x, y);
                    ++dark;
                }
            }
        const bool invert = dark * 10 > opaque * 6;
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) {
                const std::uint32_t c = file->icon[static_cast<std::size_t>(y * 32 + x)];
                if ((c >> 24) >= 128 && icon.at(x, y) != invert)
                    lcd.set(x, y);
            }
    } else {
        for (int i = 2; i < 30; ++i) {  // no icon: a frame where it would be
            lcd.set(i, 2);
            lcd.set(i, 29);
            lcd.set(2, i);
            lcd.set(29, i);
        }
    }
    // Which save of how many, then its size in blocks.
    const std::string which = std::to_string(index + 1), of = std::to_string(count),
                      blocks = std::to_string(file->blocks);
    vmu_lcd_text(lcd, 48 - vmu_lcd_text_width(which), 1, which);
    for (int x = 35; x < 48; ++x) lcd.set(x, 8);
    vmu_lcd_text(lcd, 48 - vmu_lcd_text_width(of), 10, of);
    vmu_lcd_text(lcd, 48 - vmu_lcd_text_width(blocks), 20, blocks);
    vmu_lcd_text(lcd, 48 - vmu_lcd_text_width("BL"), 26, "BL");
    return lcd;
}

// --- the card --------------------------------------------------------------------------------

int vmu_width(int height) {
    return static_cast<int>(std::lround(static_cast<float>(height) * 0.623f));
}

VmuPlacement draw_vmu(std::uint32_t* rgba, unsigned pitch, unsigned width, unsigned height, int x,
                      int y, int card_height, const VmuLcd& lcd, std::string_view label,
                      const VmuPressed& pressed) {
    Canvas cv{rgba, pitch, width, height};
    const float W = static_cast<float>(vmu_width(card_height)), H = static_cast<float>(card_height),
                X = static_cast<float>(x), Y = static_cast<float>(y);
    auto U = [&](float u) { return X + u * W; };
    auto V = [&](float v) { return Y + v * H; };
    const float line = std::max(1.0f, H / 260.0f);

    // The shell, then the two rails down its sides, which stand a little higher at the top.
    outlined_rect(cv, U(0.0f), V(0.03f), U(1.0f), V(1.0f), 0.11f * W, line, kEdge, kShell);
    outlined_rect(cv, U(0.0f), V(0.0f), U(0.135f), V(0.93f), 0.06f * W, line, kEdge, kRail);
    outlined_rect(cv, U(0.865f), V(0.0f), U(1.0f), V(0.93f), 0.06f * W, line, kEdge, kRail);

    // The round window at the top, and the curved line of the moulding below it.
    outlined_circle(cv, U(0.5f), V(0.09f), 0.094f * W, line, kWindowEdge, kWindow);
    circle(cv, U(0.53f), V(0.078f), 0.04f * W, kWhite, 0.7f);
    {
        const float radius = 1.6f * W, cx = U(0.5f), cy = V(0.162f) - radius;
        const float left = U(0.15f), right = U(0.85f);
        cv.fill(
            left, V(0.12f), right, V(0.17f),
            [&](float px, float py) {
                const float ring = std::fabs(sd_circle(px, py, cx, cy, radius)) - line * 0.5f;
                return std::max(ring, std::max(left - px, px - right));
            },
            [](float) { return kWindowEdge; });
    }

    // The name, where the maker's would be.
    const unsigned label_scale = std::max(1u, static_cast<unsigned>(H / 140.0f));
    centred_text(cv, U(0.5f), V(0.185f), fit_text(label, static_cast<int>(0.66f * W), label_scale),
                 label_scale, kLabel);

    // The screen: its bezel, the green glass with a light diagonal sheen, and the pixels.
    outlined_rect(cv, U(0.15f), V(0.252f), U(0.85f), V(0.612f), 0.035f * W, line, kWindowEdge,
                  kBezel);
    const float gx0 = U(0.17f), gy0 = V(0.272f), gx1 = U(0.83f), gy1 = V(0.593f);
    const float gr = 0.02f * W;
    cv.fill(
        gx0, gy0, gx1, gy1,
        [&](float px, float py) { return sd_round_rect(px, py, gx0, gy0, gx1, gy1, gr); },
        [&](float py) { return mix(kGlassTop, kGlassBottom, (py - gy0) / (gy1 - gy0)); });
    cv.fill(
        gx0, gy0, gx1, gy1,
        [&](float px, float py) {
            const float diagonal =
                ((px - gx0) / (gx1 - gx0) + (py - gy0) / (gy1 - gy0) - 0.62f) * (gy1 - gy0) * 0.5f;
            return std::max(sd_round_rect(px, py, gx0, gy0, gx1, gy1, gr), diagonal);
        },
        [](float) { return kWhite; }, 0.16f);
    // The pixels fill the glass but for a thin margin. A cell is not a whole number of screen
    // pixels, so each one's edges are rounded on their own, which keeps the grid even whatever the
    // card's size; a hair of gap between them, as on the glass, once they are big enough.
    const float cell = std::max(1.0f, std::min((gx1 - gx0) * 0.96f / VmuLcd::kWidth,
                                               (gy1 - gy0) * 0.95f / VmuLcd::kHeight));
    const float ox = (gx0 + gx1) * 0.5f - cell * VmuLcd::kWidth * 0.5f,
                oy = (gy0 + gy1) * 0.5f - cell * VmuLcd::kHeight * 0.5f;
    const int gap = cell >= 3.0f ? 1 : 0;
    for (int py = 0; py < VmuLcd::kHeight; ++py)
        for (int px = 0; px < VmuLcd::kWidth; ++px)
            if (lcd.at(px, py)) {
                const int x0 = static_cast<int>(std::lround(ox + static_cast<float>(px) * cell)),
                          x1 =
                              static_cast<int>(std::lround(ox + static_cast<float>(px + 1) * cell)),
                          y0 = static_cast<int>(std::lround(oy + static_cast<float>(py) * cell)),
                          y1 =
                              static_cast<int>(std::lround(oy + static_cast<float>(py + 1) * cell));
                fill_rect(rgba, pitch, width, height, x0, y0, std::max(1, x1 - x0 - gap),
                          std::max(1, y1 - y0 - gap), kPixel);
            }

    // The cross pad: a dark well, the pad, its cross, an arm darker while held.
    const float pcx = U(0.27f), pcy = V(0.745f);
    circle(cv, pcx, pcy, 0.158f * W, kPadDark);
    circle(cv, pcx, pcy, 0.13f * W, kPadFace);
    const float arm = 0.042f * W, reach = 0.112f * W;
    round_rect(cv, pcx - arm, pcy - reach, pcx + arm, pcy + reach, arm * 0.4f, kPadCross);
    round_rect(cv, pcx - reach, pcy - arm, pcx + reach, pcy + arm, arm * 0.4f, kPadCross);
    struct Arm {
        VmuButton b;
        float dx, dy;
    };
    for (const Arm& a : {Arm{VmuButton::Up, 0, -1}, Arm{VmuButton::Down, 0, 1},
                         Arm{VmuButton::Left, -1, 0}, Arm{VmuButton::Right, 1, 0}})
        if (pressed_at(pressed, a.b)) {
            const float cx = pcx + a.dx * reach * 0.6f, cy = pcy + a.dy * reach * 0.6f;
            const float hw = a.dx != 0 ? reach * 0.4f : arm, hh = a.dy != 0 ? reach * 0.4f : arm;
            round_rect(cv, cx - hw, cy - hh, cx + hw, cy + hh, arm * 0.4f, kPressed);
        }
    circle(cv, pcx, pcy, 0.018f * W, kPadFace);

    // SLEEP and MODE, then A and B, with their names printed on the shell.
    const unsigned small = std::max(1u, static_cast<unsigned>(H / 400.0f));
    struct Button {
        VmuButton b;
        float u, v, r;
        const char* name;
        float name_v;
    };
    for (const Button& b : {Button{VmuButton::Sleep, 0.52f, 0.695f, 0.036f, "SLEEP", 0.646f},
                            Button{VmuButton::Mode, 0.665f, 0.695f, 0.036f, "MODE", 0.646f},
                            Button{VmuButton::A, 0.595f, 0.795f, 0.072f, "A", 0.86f},
                            Button{VmuButton::B, 0.8f, 0.772f, 0.072f, "B", 0.837f}}) {
        outlined_circle(cv, U(b.u), V(b.v), b.r * W, line * 1.5f, kPadDark,
                        pressed_at(pressed, b.b) ? kPressed : kPadFace);
        if (!pressed_at(pressed, b.b))
            circle(cv, U(b.u) - b.r * W * 0.25f, V(b.v) - b.r * W * 0.3f, b.r * W * 0.35f, kWhite,
                   0.25f);
        centred_text(cv, U(b.u), V(b.name_v), b.name, b.r > 0.05f ? small + 1 : small, kInk);
    }

    VmuPlacement p;
    p.x = x;
    p.y = y;
    p.w = static_cast<int>(W);
    p.h = card_height;
    p.lcd_x = static_cast<int>(gx0);
    p.lcd_y = static_cast<int>(gy0);
    p.lcd_w = static_cast<int>(gx1 - gx0);
    p.lcd_h = static_cast<int>(gy1 - gy0);
    return p;
}

}  // namespace dream::render
