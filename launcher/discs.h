// The disc images in the player's dumps folder, for the launcher's AJOUTER UN JEU screen (WP3.6).
//
// Each one is matched against the library by the files themselves: a game's disc/ directory holds
// hard links to the dump's files (importer.h, adopt_disc), so a dump already added shares a file
// with it. Comparing sizes first keeps that to a handful of file-identity checks.
//
// Device-independent and unit-tested, like library.h.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "library.h"

namespace dream::launcher {

struct Disc {
    std::filesystem::path image;  // the .gdi, .cdi or .chd
    std::string name;             // its file name without the extension
    std::string game_id;          // the library's game for it, empty when not added yet
    std::string game_title;
};

// The track files a .gdi lists: "number lba type sector_size file offset" per line after the
// count, the name quoted when it has spaces. Empty when a name has a directory in it.
std::vector<std::string> gdi_tracks(const std::string& text);

// The images in `dir` and in its folders one level down, sorted by name, each with the game of
// `games` it belongs to.
std::vector<Disc> find_discs(const std::filesystem::path& dir, const std::vector<Game>& games);

}  // namespace dream::launcher
