// Adding a game from a disc image, for the launcher's "add a game" (WP3.6).
//
// The steps a person takes by hand, in order: dcdisc new-game writes the configuration (and
// refuses a Windows CE disc), CMake configures the new game and builds it, then a headless run
// with --suggest-config finds the functions discovery missed and the code the title copies
// elsewhere; those are folded into the configuration and the game built again, until a run
// suggests nothing new. The last run decides the status the library shows. A translation fault
// is not something this can fix: it ends with a status that says where the game stops.
//
// A disc already in the library is not refused: its build and test rounds are run again, which
// is how a game added before a fix is brought up to date. A status a person wrote is theirs; the
// launcher only replaces the ones it wrote itself.
//
// The programs are started through a Runner, so the sequence is tested without starting any.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace dream::launcher {

struct ImportOptions {
    std::filesystem::path disc;    // the .gdi, .cdi or .chd
    std::filesystem::path root;    // the checkout: games/ is under it
    std::filesystem::path build;   // the CMake build directory
    std::filesystem::path dcdisc;  // the dcdisc program
    std::string cmake = "cmake";
    std::filesystem::path logs;  // where each step's output goes
    unsigned rounds = 3;         // test runs, each followed by folding in what it suggested
    unsigned frames = 3000;      // guest frames per test run
};

// What the import is doing, shared with the window: the importer writes it under `mutex`.
struct ImportStatus {
    std::mutex mutex;
    std::string step;  // what runs now, in the overlay font's upper case
    std::string id;    // the game's id, once dcdisc has named it
    bool done = false, ok = false;
    std::string message;              // the outcome, for the launcher's message line
    std::filesystem::path log;        // the output of the step that failed
    std::atomic<bool> cancel{false};  // set by the window; the Runner stops what it runs
};

// How the notes the launcher writes begin, so a later import knows the status is its own.
extern const char* const kLauncherNote;

// Starts a program (arguments, program first) in `cwd` with its output in `log`, and waits for it
// at most `timeout_s` seconds. Returns its exit code, or -1 when it did not start or was stopped.
using Runner =
    std::function<int(const std::vector<std::string>& args, const std::filesystem::path& cwd,
                      const std::filesystem::path& log, unsigned timeout_s)>;

// The buttons a test run presses, to get past title screens: start, then A.
extern const char* const kImportPresses;

void import_game(const ImportOptions& options, const Runner& run, ImportStatus& status);

// The disc image given a place in the game's own disc/ directory (ignored by git), so its
// configuration says "disc/<id>.gdi" wherever the owner keeps the dump and can be shared as it
// is. The files are hard links, which take no space. Returns that path relative to `game_dir`,
// or "" when a link cannot be made (the image is on another drive): the configuration then keeps
// naming the image where it is.
std::string adopt_disc(const std::filesystem::path& game_dir, const std::string& id,
                       const std::filesystem::path& image);

// After a game was played with --suggest-config: what that run found that the configuration
// lacks, folded into it (the file is rewritten only when there is something new).
struct Findings {
    unsigned functions = 0, relocations = 0;
    std::string error;  // a file did not parse or could not be written
};
Findings fold_findings(const std::filesystem::path& config, const std::filesystem::path& suggest);

// Builds a game again after its configuration changed; `done` is the message when it worked.
// Cancelled, the old executable stays: it still runs, and the next session builds again.
void update_game(const ImportOptions& options, const std::string& id, const std::string& done,
                 const Runner& run, ImportStatus& status);

}  // namespace dream::launcher
