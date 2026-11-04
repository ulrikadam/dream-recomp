// A memory card drawn by the program rather than from a picture: front on, in the colours players
// know (a white shell with side rails, the round window at the top, the green screen, the cross
// pad, SLEEP and MODE, A and B). The shapes are filled with anti-aliased edges from their distance
// functions, so the card is sharp at any size and needs no file next to the executable.
//
// Its screen is the real one's 48x32 pixels, with a 3x5 font of its own, so what the launcher
// writes there looks like what a VMU shows. Device-independent and unit-tested, like overlay.h.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "dream/runtime/maple/vmu_fs.h"

namespace dream::render {

enum class VmuButton : unsigned { Up, Down, Left, Right, A, B, Mode, Sleep, Count };
using VmuPressed = std::array<bool, static_cast<unsigned>(VmuButton::Count)>;

// The card's screen: 48x32 pixels, true for a dark one, row by row from the top left.
struct VmuLcd {
    static constexpr int kWidth = 48, kHeight = 32;
    std::array<bool, kWidth * kHeight> px{};
    void set(int x, int y, bool on = true) {
        if (x >= 0 && y >= 0 && x < kWidth && y < kHeight)
            px[static_cast<std::size_t>(y * kWidth + x)] = on;
    }
    bool at(int x, int y) const {
        return x >= 0 && y >= 0 && x < kWidth && y < kHeight &&
               px[static_cast<std::size_t>(y * kWidth + x)];
    }
};

// Text in the screen's 3x5 font, top left at (x, y): digits, A to Z, and / - + : . and space.
// Returns the x just after it. Each glyph is 3 pixels wide with 1 between.
int vmu_lcd_text(VmuLcd& lcd, int x, int y, std::string_view text);
int vmu_lcd_text_width(std::string_view text);

// What the screen shows for the save at `index` of `count` on a card: its icon in one colour on
// the left, and on the right which save it is, how many there are, and its size in blocks. A
// null `file` is an empty card ("VIDE"); `new_card` is the place to make one ("+" and "CREER").
VmuLcd vmu_file_screen(const maple::VmuFile* file, unsigned index, unsigned count,
                       bool new_card = false);

// Where the card and its screen landed.
struct VmuPlacement {
    int x = 0, y = 0, w = 0, h = 0;
    int lcd_x = 0, lcd_y = 0, lcd_w = 0, lcd_h = 0;  // the glass
};

// Width of a card `height` pixels tall.
int vmu_width(int height);

// Draws the card with its top left at (x, y), `height` pixels tall, into an RGBA buffer (red in
// the low byte, as overlay.h). `label` is printed where the maker's name goes; `pressed` shows
// the buttons being held.
VmuPlacement draw_vmu(std::uint32_t* rgba, unsigned pitch, unsigned width, unsigned height, int x,
                      int y, int card_height, const VmuLcd& lcd, std::string_view label,
                      const VmuPressed& pressed);

}  // namespace dream::render
