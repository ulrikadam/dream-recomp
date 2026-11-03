#include <cstdint>
#include <initializer_list>

#include "dream/translator/analysis/switch.h"

#include "doctest.h"

using dream::translator::Image;
using dream::translator::recover_switch;
using dream::translator::SwitchTable;

namespace {

// SHC's absolute-table dispatch, as in Ikaruga's 0x0C08FE94:
//   +00 shll2 r1 / +02 mova tbl,r0 / +04 mov.l @(r0,r1),r0 / +06 jmp @r0 / +08 nop
//   +0C tbl: two entries / +14 rts, nop / +18 rts, nop
Image absolute_table(std::uint32_t base, std::uint32_t entry_segment) {
    Image img;
    img.base = base;
    auto put16 = [&](std::uint16_t v) {
        img.bytes.push_back(static_cast<std::uint8_t>(v));
        img.bytes.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    for (std::uint16_t w :
         std::initializer_list<std::uint16_t>{0x4108, 0xC702, 0x001E, 0x402B, 0x0009, 0x0000})
        put16(w);
    for (std::uint32_t off : {0x14u, 0x18u}) {
        const std::uint32_t entry = ((base + off) & 0x1FFFFFFFu) | entry_segment;
        put16(static_cast<std::uint16_t>(entry));
        put16(static_cast<std::uint16_t>(entry >> 16));
    }
    for (std::uint16_t w : std::initializer_list<std::uint16_t>{0x000B, 0x0009, 0x000B, 0x0009})
        put16(w);
    return img;
}

bool recover(const Image& img, SwitchTable& sw) {
    const std::uint32_t jump_pc = img.base + 6;
    return recover_switch(img, img.base, jump_pc, dream::sh4::decode(img.read16(jump_pc)), img.base,
                          img.base + static_cast<std::uint32_t>(img.bytes.size()), sw);
}

}  // namespace

TEST_CASE("switch: absolute P1 entries are rebased into an image modelled at 0x0C") {
    const Image img = absolute_table(0x0C010000u, 0x80000000u);
    SwitchTable sw;
    REQUIRE(recover(img, sw));
    CHECK(sw.table == 0x0C01000Cu);
    CHECK(sw.rebased);
    REQUIRE(sw.targets.size() == 2);
    CHECK(sw.targets[0] == 0x0C010014u);
    CHECK(sw.targets[1] == 0x0C010018u);
}

TEST_CASE("switch: absolute entries in the image's own segment are not rebased") {
    const Image img = absolute_table(0x8C010000u, 0x80000000u);
    SwitchTable sw;
    REQUIRE(recover(img, sw));
    CHECK_FALSE(sw.rebased);
    REQUIRE(sw.targets.size() == 2);
    CHECK(sw.targets[0] == 0x8C010014u);
}

TEST_CASE("switch: P4 entries are not an alias of RAM") {
    const Image img = absolute_table(0x0C010000u, 0xE0000000u);
    SwitchTable sw;
    CHECK_FALSE(recover(img, sw));
}

TEST_CASE("switch: the index check bounds the table") {
    // +00 mov #1,r2 / +02 cmp/hi r2,r1 / +04 nop / +06 shll2 r1 / +08 mova tbl,r0
    // +0A mov.l @(r0,r1),r0 / +0C jmp @r0 / +0E nop / +14 tbl: three in-range words / +20.. rts
    Image img;
    img.base = 0x8C010000u;
    auto put16 = [&](std::uint16_t v) {
        img.bytes.push_back(static_cast<std::uint8_t>(v));
        img.bytes.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    for (std::uint16_t w : std::initializer_list<std::uint16_t>{
             0xE201, 0x3126, 0x0009, 0x4108, 0xC702, 0x001E, 0x402B, 0x0009, 0x0000, 0x0000})
        put16(w);
    for (std::uint32_t off : {0x20u, 0x24u, 0x28u}) {
        put16(static_cast<std::uint16_t>(img.base + off));
        put16(static_cast<std::uint16_t>((img.base + off) >> 16));
    }
    for (std::uint16_t w :
         std::initializer_list<std::uint16_t>{0x000B, 0x0009, 0x000B, 0x0009, 0x000B, 0x0009})
        put16(w);
    const std::uint32_t jump_pc = img.base + 0x0C;
    SwitchTable sw;
    REQUIRE(recover_switch(img, img.base, jump_pc, dream::sh4::decode(img.read16(jump_pc)),
                           img.base, img.base + static_cast<std::uint32_t>(img.bytes.size()), sw));
    REQUIRE(sw.targets.size() == 2);
    CHECK(sw.targets[1] == 0x8C010024u);
}

TEST_CASE("switch: a byte table read through extu.b holds unsigned offsets") {
    // GCC's shape in F355 Challenge (0x0c097f5a) and 18 Wheeler (0x0c133946):
    //   mov #4,r2 ; cmp/hi r2,r1 ; nop ; mova tbl,r0 ; mov.b @(r0,r1),r1 ; extu.b r1,r1 ;
    //   braf r1 ; nop ; tbl: .byte 6,8,10,12,0x86
    // The extu.b used to stop the walk back to the table load, and 0x86 is past 0x7F: read
    // signed, it would land before the table.
    Image img;
    img.base = 0x0C010000u;
    for (std::uint16_t w : {0xE204, 0x3126, 0x0009, 0xC702, 0x011C, 0x611C, 0x0123, 0x0009}) {
        img.bytes.push_back(static_cast<std::uint8_t>(w));
        img.bytes.push_back(static_cast<std::uint8_t>(w >> 8));
    }
    for (std::uint8_t b : {0x06, 0x08, 0x0A, 0x0C, 0x86, 0x00}) img.bytes.push_back(b);
    while (img.bytes.size() < 0xA0) {
        img.bytes.push_back(0x09);  // nop
        img.bytes.push_back(0x00);
    }
    const std::uint32_t jump_pc = img.base + 0x0C;
    SwitchTable sw;
    REQUIRE(recover_switch(img, img.base, jump_pc, dream::sh4::decode(img.read16(jump_pc)),
                           img.base, img.base + 0xA0, sw));
    CHECK(sw.table == img.base + 0x10);
    CHECK(sw.entry_size == 1);
    REQUIRE(sw.targets.size() == 5);  // the cmp/hi bound: indices 0 to 4
    CHECK(sw.targets[0] == img.base + 0x16);
    CHECK(sw.targets[3] == img.base + 0x1C);
    CHECK(sw.targets[4] == img.base + 0x96);
}
