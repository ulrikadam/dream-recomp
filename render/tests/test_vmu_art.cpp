// The memory card drawn by the program, and what its screen shows.
#include <cstdint>
#include <vector>

#include "dream/render/vmu_art.h"

#include "doctest.h"

using namespace dream::render;

namespace {

constexpr unsigned kW = 400, kH = 600;
constexpr std::uint32_t kBlack = 0xFF000000u;

struct Shot {
    std::vector<std::uint32_t> px = std::vector<std::uint32_t>(kW * kH, kBlack);
    VmuPlacement p;
    Shot(const VmuLcd& lcd, const VmuPressed& pressed = {}, int x = 40, int y = 20) {
        p = draw_vmu(px.data(), kW, kW, kH, x, y, 540, lcd, "VMU 1", pressed);
    }
    std::uint32_t at(int x, int y) const { return px[static_cast<std::size_t>(y) * kW + x]; }
};

unsigned red(std::uint32_t c) {
    return c & 0xFF;
}
unsigned green(std::uint32_t c) {
    return (c >> 8) & 0xFF;
}
unsigned blue(std::uint32_t c) {
    return (c >> 16) & 0xFF;
}

}  // namespace

TEST_CASE("vmu art: a card with a green screen, drawn where it was asked, nothing outside") {
    const Shot s{VmuLcd{}};
    CHECK(s.p.w == vmu_width(540));
    CHECK(s.p.w > 300);
    CHECK(s.p.w < 360);
    // The glass is inside the card, and green.
    CHECK(s.p.lcd_x > s.p.x);
    CHECK(s.p.lcd_x + s.p.lcd_w < s.p.x + s.p.w);
    const std::uint32_t glass = s.at(s.p.lcd_x + s.p.lcd_w / 2, s.p.lcd_y + s.p.lcd_h - 4);
    CHECK(green(glass) > red(glass) + 40);
    CHECK(green(glass) > blue(glass) + 10);
    // The shell is light, and the space around the card untouched.
    const std::uint32_t shell = s.at(s.p.x + s.p.w / 2, s.p.y + s.p.h - 10);
    CHECK(red(shell) > 200);
    CHECK(s.at(5, 5) == kBlack);
    CHECK(s.at(kW - 2, kH - 2) == kBlack);
}

TEST_CASE("vmu art: a lit pixel of the screen is dark, and a held button changes colour") {
    VmuLcd lcd;
    lcd.set(0, 0);
    const Shot lit{lcd};
    const Shot dark{VmuLcd{}};
    // The first pixel sits near the glass's top left corner; find the darkest spot there.
    unsigned darkest = 255;
    for (int y = lit.p.lcd_y; y < lit.p.lcd_y + lit.p.lcd_h / 4; ++y)
        for (int x = lit.p.lcd_x; x < lit.p.lcd_x + lit.p.lcd_w / 4; ++x)
            darkest = std::min(darkest, green(lit.at(x, y)));
    CHECK(darkest < 80);
    VmuPressed a{};
    a[static_cast<unsigned>(VmuButton::A)] = true;
    const Shot held{VmuLcd{}, a};
    const int ax = held.p.x + static_cast<int>(0.595f * held.p.w),
              ay = held.p.y + static_cast<int>(0.795f * held.p.h) + 3;
    CHECK(held.at(ax, ay) != dark.at(ax, ay));
}

TEST_CASE("vmu art: drawn partly off the buffer, it is clipped rather than written past") {
    std::vector<std::uint32_t> px(100 * 100 + 16, 0x12345678u);
    draw_vmu(px.data(), 100, 100, 100, -150, -200, 540, VmuLcd{}, "VMU 1", {});
    for (std::size_t i = 100 * 100; i < px.size(); ++i) CHECK(px[i] == 0x12345678u);
}

TEST_CASE("vmu art: the screen's font, and what a save, an empty card and a new one show") {
    VmuLcd lcd;
    CHECK(vmu_lcd_text(lcd, 0, 0, "10") == 8);
    CHECK(vmu_lcd_text_width("10") == 7);
    CHECK(lcd.at(1, 0));   // the 1's stem
    CHECK(lcd.at(4, 0));   // the 0's top
    CHECK(!lcd.at(5, 2));  // the 0's hollow
    CHECK(!lcd.at(3, 0));  // the gap between them

    dream::maple::VmuFile f;
    f.blocks = 60;
    f.icon.assign(32 * 32, 0u);
    f.icon[0] = 0xFF000000u;  // opaque black: lit
    f.icon[1] = 0xFFFFFFFFu;  // opaque white: not
    f.icon[2] = 0x00000000u;  // transparent: not
    const VmuLcd save = vmu_file_screen(&f, 1, 5);
    CHECK(save.at(0, 0));
    CHECK(!save.at(1, 0));
    CHECK(!save.at(2, 0));
    // "2" over "5" on the right, then the blocks.
    CHECK(save.at(45, 1));
    CHECK(save.at(40, 8));

    const VmuLcd empty = vmu_file_screen(nullptr, 0, 0);
    const VmuLcd fresh = vmu_file_screen(nullptr, 0, 0, true);
    unsigned lit_empty = 0, lit_fresh = 0;
    for (bool b : empty.px) lit_empty += b;
    for (bool b : fresh.px) lit_fresh += b;
    CHECK(lit_empty > 10);
    CHECK(lit_fresh > lit_empty);
    CHECK(fresh.at(24, 11));  // the plus's centre
}
