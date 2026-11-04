// The launcher's shared memory cards, and gathering the old one-per-title cards onto them.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "dream/runtime/maple/vmu_fs.h"

#include "../cards.h"
#include "doctest.h"

namespace fs = std::filesystem;
using namespace dream::launcher;

namespace {

struct Saves {
    fs::path dir = fs::temp_directory_path() / "dream_cards_test";
    Saves() {
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Saves() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

void put16(std::vector<std::uint8_t>& v, std::size_t off, unsigned x) {
    v[off] = static_cast<std::uint8_t>(x & 0xFF);
    v[off + 1] = static_cast<std::uint8_t>(x >> 8);
}

// A card with one save of `blocks` blocks, every byte `fill`, as a title would leave it.
std::vector<std::uint8_t> card_with(const std::string& name, unsigned blocks, std::uint8_t fill) {
    std::vector<std::uint8_t> img = dream::maple::blank_vmu();
    const std::size_t fat = 254 * 512;
    for (unsigned i = 0; i < blocks; ++i) {
        const unsigned b = 199 - i;
        put16(img, fat + b * 2, i + 1 < blocks ? b - 1 : 0xFFFA);
        std::fill(img.begin() + b * 512, img.begin() + (b + 1) * 512, fill);
    }
    std::uint8_t* entry = img.data() + 253 * 512;
    entry[0] = 0x33;
    put16(img, 253 * 512 + 2, 199);
    for (std::size_t i = 0; i < 12; ++i)
        entry[4 + i] = static_cast<std::uint8_t>(i < name.size() ? name[i] : ' ');
    put16(img, 253 * 512 + 0x18, blocks);
    return img;
}

// Adds a second save to a card made by card_with, below the first.
void add_save(std::vector<std::uint8_t>& img, const std::string& name, unsigned blocks,
              std::uint8_t fill) {
    const std::vector<std::uint8_t> other = card_with(name, blocks, fill);
    REQUIRE(dream::maple::copy_vmu_file(other, name, img) == dream::maple::VmuEdit::Done);
}

void write(const fs::path& p, const std::vector<std::uint8_t>& img, int age_minutes) {
    std::ofstream(p, std::ios::binary)
        .write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
    fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::minutes(age_minutes));
}

// The first byte of a save's first block on a card, to tell copies apart.
std::uint8_t fill_of(const std::vector<std::uint8_t>& img, const std::string& name) {
    for (unsigned k = 0; k < 13; ++k)
        for (unsigned e = 0; e < 16; ++e) {
            const std::size_t off = (253 - k) * 512 + e * 32;
            if (img[off] == 0x33 &&
                std::string(img.begin() + off + 4, img.begin() + off + 4 + name.size()) == name)
                return img[(img[off + 2] | img[off + 3] << 8) * 512];
        }
    return 0;
}

}  // namespace

TEST_CASE("cards: shared cards are vmu-<n>.bin, listed by number before the older ones") {
    CHECK(card_number("vmu-1.bin") == 1);
    CHECK(card_number("vmu-12.bin") == 12);
    CHECK(card_number("vmu-01.bin") == 0);
    CHECK(card_number("vmu-.bin") == 0);
    CHECK(card_number("rush2049-vmu.bin") == 0);
    Saves s;
    const std::vector<std::uint8_t> blank = dream::maple::blank_vmu();
    for (const char* n : {"vmu-10.bin", "vmu-2.bin", "vmu-1.bin", "f355-vmu.bin"})
        write(s.dir / n, blank, 0);
    std::ofstream(s.dir / "small.bin") << "not a card";
    std::ofstream(s.dir / "notes.txt") << "nothing";
    CHECK(list_cards(s.dir) ==
          std::vector<std::string>{"vmu-1.bin", "vmu-2.bin", "vmu-10.bin", "f355-vmu.bin"});
    CHECK(next_card(s.dir) == "vmu-3.bin");
}

TEST_CASE("cards: a card is made blank, and writing one keeps the version it replaces") {
    Saves s;
    REQUIRE(create_card(s.dir / "vmu-1.bin"));
    CHECK_FALSE(create_card(s.dir / "vmu-1.bin"));  // never over an existing card
    CHECK(dream::maple::read_vmu(read_card(s.dir / "vmu-1.bin")).free_blocks == 200);
    REQUIRE(write_card(s.dir / "vmu-1.bin", card_with("SAVE", 3, 7)));
    CHECK(dream::maple::read_vmu(read_card(s.dir / "vmu-1.bin")).files.size() == 1);
    CHECK(dream::maple::read_vmu(read_card(s.dir / "vmu-1.bin.bak")).files.empty());
    CHECK_FALSE(write_card(s.dir / "vmu-1.bin", std::vector<std::uint8_t>(10)));
    CHECK(read_card(s.dir / "missing.bin").empty());
}

TEST_CASE("cards: the old cards' saves are gathered onto VMU 1, the newest copy of each") {
    Saves s;
    std::vector<std::uint8_t> a = card_with("SAVE_A", 10, 1);
    add_save(a, "SHARED", 5, 2);  // the older copy
    std::vector<std::uint8_t> b = card_with("SAVE_B", 20, 3);
    add_save(b, "SHARED", 5, 4);  // the newer copy
    write(s.dir / "aaa-vmu.bin", a, 60);
    write(s.dir / "bbb-vmu.bin", b, 5);
    write(s.dir / "empty-vmu.bin", dream::maple::blank_vmu(), 1);
    const Gathered g = gather_saves(s.dir);
    CHECK(g.made);
    CHECK(g.copied == 3);
    CHECK(g.skipped == 1);
    CHECK(g.cards == std::vector<std::string>{"vmu-1.bin"});
    const std::vector<std::uint8_t> one = read_card(s.dir / "vmu-1.bin");
    const auto c = dream::maple::read_vmu(one);
    CHECK(c.files.size() == 3);
    CHECK(c.free_blocks == 200 - 35);
    CHECK(fill_of(one, "SHARED") == 4);
    CHECK(fill_of(one, "SAVE_A") == 1);
    // The old cards are put away as they were, out of the lists, and it happens once.
    CHECK(g.moved == 3);
    CHECK(read_card(s.dir / kOldCards / "aaa-vmu.bin") == a);
    CHECK_FALSE(fs::exists(s.dir / "aaa-vmu.bin"));
    CHECK(list_cards(s.dir) == std::vector<std::string>{"vmu-1.bin"});
    CHECK_FALSE(gather_saves(s.dir).made);
}

TEST_CASE("cards: when VMU 1 fills, gathering goes on to the next free card") {
    Saves s;
    write(s.dir / "one-vmu.bin", card_with("ONE", 90, 1), 30);
    write(s.dir / "two-vmu.bin", card_with("TWO", 90, 2), 20);
    write(s.dir / "three-vmu.bin", card_with("THREE", 90, 3), 10);
    write(s.dir / "vmu-2.bin", dream::maple::blank_vmu(), 0);  // the player's own, kept
    const Gathered g = gather_saves(s.dir);
    CHECK(g.copied == 3);
    CHECK(g.cards == std::vector<std::string>{"vmu-1.bin", "vmu-3.bin"});
    CHECK(dream::maple::read_vmu(read_card(s.dir / "vmu-1.bin")).files.size() == 2);
    CHECK(dream::maple::read_vmu(read_card(s.dir / "vmu-3.bin")).files.size() == 1);
    CHECK(dream::maple::read_vmu(read_card(s.dir / "vmu-2.bin")).files.empty());
}

TEST_CASE("cards: with no old cards, VMU 1 is made empty") {
    Saves s;
    const Gathered g = gather_saves(s.dir);
    CHECK(g.made);
    CHECK(g.copied == 0);
    CHECK(dream::maple::read_vmu(read_card(s.dir / "vmu-1.bin")).formatted);
}
