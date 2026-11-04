// The launcher's memory cards (WP3.6).
//
// A real VMU holds saves from many titles, and a player keeps a few of them. The launcher works the
// same way: shared cards named vmu-<n>.bin in the save directory, which any title can use, with
// vmu-1.bin the one every title starts with. Before them each title had a card of its own,
// <id>-vmu.bin; gather_saves brings those saves together onto the shared cards once, and puts the
// old files away in a folder of their own, kOldCards, untouched.
//
// Device-independent and unit-tested, like library.h.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace dream::launcher {

// The card every title uses until the player chooses another.
inline constexpr const char* kFirstCard = "vmu-1.bin";
// Where gather_saves puts the old one-per-title cards, inside the save directory. The name is the
// launcher's language, as its screens are.
inline constexpr const char* kOldCards = "anciennes-cartes";

// n for vmu-<n>.bin, 0 for any other name.
unsigned card_number(std::string_view file);

// The card files in `saves` (128 KB images): shared cards by number, then the others by name.
std::vector<std::string> list_cards(const std::filesystem::path& saves);

// The file name of the next shared card: vmu-<n>.bin with the smallest n not taken.
std::string next_card(const std::filesystem::path& saves);

// A card's image, or empty when the file is not a 128 KB card.
std::vector<std::uint8_t> read_card(const std::filesystem::path& path);

// Writes a card back. The version it replaces is kept beside it as <file>.bak, so one wrong
// delete can be undone by hand.
bool write_card(const std::filesystem::path& path, const std::vector<std::uint8_t>& image);

// Makes a blank formatted card; false when the file exists already or cannot be written.
bool create_card(const std::filesystem::path& path);

struct Gathered {
    bool made = false;               // vmu-1.bin was made by this call
    unsigned copied = 0;             // saves copied onto the shared cards
    unsigned skipped = 0;            // saves left out: a newer one of the same name, or no room
    unsigned moved = 0;              // old cards put away in kOldCards
    std::vector<std::string> cards;  // the shared cards written
};

// Once, when vmu-1.bin does not exist yet: makes it and copies onto it every save from the older
// cards in `saves`, going on to vmu-2.bin when it fills. The most recently written card goes
// first, so a save kept on two cards (a title's own card and a test copy) comes from the newer.
// Copy protection is not applied: these are the player's own saves moving between their cards.
// The old cards are then moved, unchanged, into `saves`/kOldCards, so the lists show the shared
// cards only; one already there of the same name stays where it was.
Gathered gather_saves(const std::filesystem::path& saves);

}  // namespace dream::launcher
