// The game library behind the launcher (WP3.6): which games this checkout has, how far each one
// gets, and whether it is built and has its disc.
//
// Device-independent: it reads each game's configuration and looks for files, and nothing here
// knows about SDL, so it is unit-tested on every runner. A game is a directory under games/ with
// its <id>.toml; its status comes from an optional [status] table there:
//
//   [status]
//   level = "playable"   # playable, ingame, boots, nothing, unsupported
//   note = "Menus, memory card, races and sound."
//
// A Windows CE title is not translated: dream-wince (tools/wince in the build) runs the
// executable on its extracted disc, which an optional [wince] table names, relative to the
// configuration:
//
//   [wince]
//   executable = "extracted/fs/RALLY2.EXE"
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dream::launcher {

// How far a game gets, best last. Unknown is a game nobody has written a status for yet.
enum class Level { Unknown, Unsupported, Nothing, Boots, InGame, Playable };

// "playable" -> Level::Playable. Anything else, the empty string included, is Unknown.
Level parse_level(std::string_view text);
// What the launcher shows for a level, in the overlay font's upper case.
std::string_view level_label(Level level);

struct Game {
    std::string id, title, region, product, note;
    Level level = Level::Unknown;
    std::filesystem::path config;  // games/<id>/<id>.toml
    std::filesystem::path exe;     // what runs it: where the build puts <id>_boot, or dream-wince
    std::filesystem::path wince;   // a Windows CE title's executable; empty for the others
    std::filesystem::path image;   // the disc image the configuration names
    bool built = false;            // that executable exists (and the title's, for WinCE)
    bool disc = false;             // the disc image exists
};

// The executable's name for a game id, with the platform's suffix.
std::string boot_executable(std::string_view id);
// dream-wince's file name, with the platform's suffix.
std::string wince_executable();

// One game from its configuration file. `build_dir` is the CMake build directory, where the
// executable is looked for. Empty, with the reason in `error`, when the file does not parse or
// lacks game.id or game.title.
std::optional<Game> read_game(const std::filesystem::path& config,
                              const std::filesystem::path& build_dir, std::string* error = nullptr);

// Every games/<id>/<id>.toml under `games_dir`, sorted by title. Files that do not read are left
// out and described, one per line, in `errors`.
std::vector<Game> scan(const std::filesystem::path& games_dir,
                       const std::filesystem::path& build_dir, std::string* errors = nullptr);

}  // namespace dream::launcher
