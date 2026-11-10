// Reading a memory card's file system for display, and keeping what a title draws on its screen.
#include <array>
#include <cstdint>
#include <vector>

#include "dream/runtime/maple/maple.h"
#include "dream/runtime/maple/vmu_fs.h"

#include "doctest.h"

using dream::maple::MemoryCard;

namespace {

constexpr std::size_t kBlock = MemoryCard::kBlockSize;

void put16(std::vector<std::uint8_t>& v, std::size_t off, std::uint16_t x) {
    v[off] = static_cast<std::uint8_t>(x & 0xFF);
    v[off + 1] = static_cast<std::uint8_t>(x >> 8);
}

// A formatted card with one two-block save at 199 -> 198, as a title would leave it.
std::vector<std::uint8_t> card_with_one_save() {
    MemoryCard card;
    card.format();
    std::vector<std::uint8_t> img = card.image();
    const std::size_t fat = 254 * kBlock;
    put16(img, fat + 199 * 2, 198);
    put16(img, fat + 198 * 2, 0xFFFA);
    std::uint8_t* entry = img.data() + 253 * kBlock;
    entry[0] = 0x33;  // a data file
    put16(img, 253 * kBlock + 2, 199);
    const char name[] = "RUSH2049    ";
    for (int i = 0; i < 12; ++i) entry[4 + i] = static_cast<std::uint8_t>(name[i]);
    put16(img, 253 * kBlock + 0x18, 2);
    // The header in the first block: a description, one icon, a palette and the bitmap, which runs
    // on into the second block of the chain.
    const std::size_t h = 199 * kBlock;
    const char desc[] = "RUSH 2049 SAVE  ";
    for (int i = 0; i < 16; ++i) img[h + i] = static_cast<std::uint8_t>(desc[i]);
    put16(img, h + 0x40, 1);
    put16(img, h + 0x60 + 2, 0xF0F0);                  // palette entry 1: opaque green
    img[h + 0x80] = 0x10;                              // pixel 0 is entry 1, pixel 1 is entry 0
    img[198 * kBlock + (0x80 + 500 - kBlock)] = 0x01;  // pixel 1001, in the chain's second block
    return img;
}

}  // namespace

TEST_CASE("a blank card lists nothing and has its whole save area free") {
    MemoryCard card;
    card.format();
    const auto c = dream::maple::read_vmu(card.image());
    CHECK(c.formatted);
    CHECK(c.user_blocks == 200);
    CHECK(c.free_blocks == 200);
    CHECK(c.files.empty());
}

TEST_CASE("an unformatted image is reported as such") {
    const std::vector<std::uint8_t> blank(MemoryCard::kImageSize, 0);
    CHECK_FALSE(dream::maple::read_vmu(blank).formatted);
    CHECK_FALSE(dream::maple::read_vmu(std::vector<std::uint8_t>(100, 0x55)).formatted);
}

TEST_CASE("a save is listed with its description and icon, following its chain") {
    const auto c = dream::maple::read_vmu(card_with_one_save());
    REQUIRE(c.files.size() == 1);
    const auto& f = c.files[0];
    CHECK(f.name == "RUSH2049");
    CHECK(f.description == "RUSH 2049 SAVE");
    CHECK_FALSE(f.game);
    CHECK(f.blocks == 2);
    CHECK(c.free_blocks == 198);
    REQUIRE(f.icon.size() == 32 * 32);
    CHECK(f.icon[0] == 0xFF00FF00u);  // opaque green, red in the low byte
    CHECK(f.icon[1] == 0u);           // entry 0: transparent black
    CHECK(f.icon[1001] == 0xFF00FF00u);
}

TEST_CASE("a save copies to another card whole, into the top of its free area") {
    using dream::maple::VmuEdit;
    const std::vector<std::uint8_t> from = card_with_one_save();
    std::vector<std::uint8_t> to = dream::maple::blank_vmu();
    // Something already at the top of the destination, so the copy has to go round it.
    put16(to, 254 * kBlock + 199 * 2, 0xFFFA);
    to[253 * kBlock + 32] = 0x33;
    put16(to, 253 * kBlock + 32 + 2, 199);
    put16(to, 253 * kBlock + 32 + 0x18, 1);
    REQUIRE(dream::maple::copy_vmu_file(from, "RUSH2049", to) == VmuEdit::Done);
    const auto c = dream::maple::read_vmu(to);
    REQUIRE(c.files.size() == 2);
    // The first free directory slot is the first one: the other file sits in the second.
    const auto& f = c.files[0];
    CHECK(f.name == "RUSH2049");
    CHECK(f.description == "RUSH 2049 SAVE");
    CHECK(f.blocks == 2);
    CHECK(f.icon == dream::maple::read_vmu(from).files[0].icon);  // both blocks came across
    CHECK(c.free_blocks == 197);
    // Blocks 198 and 197, chained, and the entry pointing at the first.
    CHECK(to[253 * kBlock + 2] == 198);
    CHECK(to[254 * kBlock + 198 * 2] == 197);
    CHECK(to[254 * kBlock + 197 * 2] == 0xFA);
    // A title keeps one file of a name: a second copy is refused, and the card left as it was.
    const std::vector<std::uint8_t> before = to;
    CHECK(dream::maple::copy_vmu_file(from, "RUSH2049", to) == VmuEdit::Exists);
    CHECK(to == before);
    CHECK(dream::maple::copy_vmu_file(from, "NOSUCHFILE", to) == VmuEdit::NotFound);
}

TEST_CASE("a copy is refused when the card is full, protected, a mini-game, or not a card") {
    using dream::maple::VmuEdit;
    std::vector<std::uint8_t> from = card_with_one_save();
    std::vector<std::uint8_t> full = dream::maple::blank_vmu();
    for (unsigned b = 1; b < 200; ++b) put16(full, 254 * kBlock + b * 2, 0xFFFA);  // one free
    CHECK(dream::maple::copy_vmu_file(from, "RUSH2049", full) == VmuEdit::NoSpace);
    std::vector<std::uint8_t> crowded = dream::maple::blank_vmu();
    for (unsigned k = 0; k < 13; ++k)
        for (unsigned e = 0; e < 16; ++e) crowded[(253 - k) * kBlock + e * 32] = 0x33;
    CHECK(dream::maple::copy_vmu_file(from, "RUSH2049", crowded) == VmuEdit::DirectoryFull);
    std::vector<std::uint8_t> to = dream::maple::blank_vmu();
    from[253 * kBlock + 1] = 0xFF;  // copy-protected
    CHECK(dream::maple::read_vmu(from).files[0].no_copy);
    CHECK(dream::maple::copy_vmu_file(from, "RUSH2049", to) == VmuEdit::Protected);
    CHECK(dream::maple::copy_vmu_file(from, "RUSH2049", to, true) == VmuEdit::Done);
    from[253 * kBlock] = 0xCC;  // a mini-game
    CHECK(dream::maple::copy_vmu_file(from, "RUSH2049", to, true) == VmuEdit::MiniGame);
    std::vector<std::uint8_t> junk(MemoryCard::kImageSize, 0);
    CHECK(dream::maple::copy_vmu_file(from, "RUSH2049", junk) == VmuEdit::NotACard);
}

TEST_CASE("a deleted save frees its blocks and its directory slot") {
    using dream::maple::VmuEdit;
    std::vector<std::uint8_t> img = card_with_one_save();
    CHECK(dream::maple::delete_vmu_file(img, "RUSH2049") == VmuEdit::Done);
    const auto c = dream::maple::read_vmu(img);
    CHECK(c.files.empty());
    CHECK(c.free_blocks == 200);
    CHECK(dream::maple::delete_vmu_file(img, "RUSH2049") == VmuEdit::NotFound);
    // The space is usable again.
    CHECK(dream::maple::copy_vmu_file(card_with_one_save(), "RUSH2049", img) == VmuEdit::Done);
    CHECK(dream::maple::read_vmu(img).free_blocks == 198);
}

TEST_CASE("the card keeps what a title draws on its screen") {
    MemoryCard card;
    std::array<std::uint32_t, 2 + MemoryCard::kLcdBytes / 4> args{};
    args[0] = dream::maple::kLcd;
    for (std::size_t i = 2; i < args.size(); ++i)
        args[i] = 0x01020304u * static_cast<std::uint32_t>(i);
    dream::maple::Payload out;
    CHECK(card.handle(dream::maple::kBlockWrite, args.data(), args.size(), out) ==
          dream::maple::kDeviceReply);
    CHECK(card.screen_writes == 1);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(args.data() + 2);
    for (std::size_t i = 0; i < MemoryCard::kLcdBytes; ++i) CHECK(card.lcd()[i] == bytes[i]);
}

TEST_CASE("the card describes its screen when asked") {
    // Windows CE's Maple driver asks for the screen's media info before a title may draw on it,
    // and keeps the card busy retrying while it is refused.
    MemoryCard card;
    const std::array<std::uint32_t, 2> args{dream::maple::kLcd, 0};
    dream::maple::Payload out;
    REQUIRE(card.handle(dream::maple::kGetMediaInfo, args.data(), args.size(), out) ==
            dream::maple::kDataTransfer);
    const auto& b = out.bytes();
    REQUIRE(b.size() == 8);
    CHECK((b[0] | b[1] << 8 | b[2] << 16 | static_cast<std::uint32_t>(b[3]) << 24) ==
          dream::maple::kLcd);
    CHECK(b[4] == 47);  // 48 dots across, less one
    CHECK(b[5] == 31);  // 32 down, less one
    // Only the one partition.
    const std::array<std::uint32_t, 2> other{dream::maple::kLcd, 1};
    dream::maple::Payload refused;
    CHECK(card.handle(dream::maple::kGetMediaInfo, other.data(), other.size(), refused) ==
          dream::maple::kUnknownCommand);
}
