// Editing a game's configuration in place, for the launcher's "add a game" (WP3.6).
//
// A run with --suggest-config writes what it earned: call targets discovery never reached
// ([functions] extra) and code the title copied and ran elsewhere ([[relocations]]). Bringing a
// title up is mostly folding those into its TOML and building again; these functions do the
// folding on the file's text, so the comments dcdisc wrote and anything a person added stay as
// they are.
//
// Device-independent and unit-tested, like library.h.
#pragma once

#include <string>
#include <string_view>

namespace dream::launcher {

struct MergeResult {
    std::string text;          // the configuration with the suggestions folded in
    unsigned functions = 0;    // extra functions it did not have
    unsigned relocations = 0;  // relocations it did not have
    std::string error;         // non-empty when either text did not parse; `text` is unchanged
};

// Folds `suggested` (a --suggest-config file) into `config` (a game's TOML): new extra functions
// join [functions] extra, sorted and without duplicates, and new relocations are appended.
MergeResult merge_suggestions(const std::string& config, const std::string& suggested);

// `config` with its [status] table replaced, or added: level is one of playable, ingame, boots,
// nothing or unsupported (library.h), note a line of text.
std::string set_status(const std::string& config, std::string_view level, std::string_view note);

// `config` with [disc] image set to `image` (a path relative to the configuration's directory).
// Unchanged when it has no [disc] image line.
std::string set_disc_image(const std::string& config, std::string_view image);

}  // namespace dream::launcher
