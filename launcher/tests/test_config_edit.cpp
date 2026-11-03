// Folding a run's suggestions and a status into a game's configuration text.
#include <string>
#include <toml.hpp>

#include "../config_edit.h"
#include "doctest.h"

using namespace dream::launcher;

namespace {

// What dcdisc new-game writes, trimmed.
const std::string kFresh =
    "# Written by `dcdisc new-game`.\n"
    "\n[game]\nid = \"demo\"\ntitle = \"Demo\"\n"
    "\n[binary]\npath = \"extracted/fs/1ST_READ.BIN\"\n"
    "\n# Addresses discovery does not reach go here.\n# [functions]\n# extra = []\n";

// What --suggest-config writes.
const std::string kSuggested =
    "# Suggested by demo_boot from a run that stopped: frame limit.\n"
    "[functions]\nextra = [0x0C0244A0,  # called 1090 times\n"
    "         0x0C0244C0]  # called 10 times\n"
    "\n# Regions the program copied out of the image and ran from somewhere else.\n"
    "[[relocations]]  # RAM 0x8c000000 reproduces image 0x0c168194 for 52 bytes\n"
    "source = 0x0C168194\nsize = 0x34\ndest = 0x8C000000\n";

}  // namespace

TEST_CASE("config: suggestions fill a fresh config's commented [functions] and add relocations") {
    const MergeResult r = merge_suggestions(kFresh, kSuggested);
    REQUIRE(r.error.empty());
    CHECK(r.functions == 2);
    CHECK(r.relocations == 1);
    const toml::parse_result t = toml::parse(r.text);
    REQUIRE(t);
    const toml::array* extra = t.table()["functions"]["extra"].as_array();
    REQUIRE(extra);
    CHECK(extra->size() == 2);
    CHECK(t.table()["relocations"].as_array()->size() == 1);
    CHECK(r.text.find("# Written by `dcdisc new-game`.") == 0);  // comments kept
}

TEST_CASE("config: a second round only adds what is new") {
    const MergeResult once = merge_suggestions(kFresh, kSuggested);
    const std::string more =
        "[functions]\nextra = [0x0C0244C0, 0x0C024502]\n"
        "[[relocations]]\nsource = 0x0C168194\nsize = 0x34\ndest = 0x8C000000\n"
        "[[relocations]]\nsource = 0x0C168100\nsize = 0x34\ndest = 0x8C000100\n";
    const MergeResult twice = merge_suggestions(once.text, more);
    REQUIRE(twice.error.empty());
    CHECK(twice.functions == 1);
    CHECK(twice.relocations == 1);
    const toml::parse_result t = toml::parse(twice.text);
    REQUIRE(t);
    CHECK(t.table()["functions"]["extra"].as_array()->size() == 3);
    CHECK(t.table()["relocations"].as_array()->size() == 2);
    // Nothing new: nothing changes.
    const MergeResult again = merge_suggestions(twice.text, more);
    CHECK(again.functions == 0);
    CHECK(again.relocations == 0);
    CHECK(again.text == twice.text);
}

TEST_CASE("config: a status is added, then replaced in place of the old one") {
    std::string text = set_status(kFresh, "boots", "Added by the launcher.");
    toml::parse_result t = toml::parse(text);
    REQUIRE(t);
    CHECK(t.table()["status"]["level"].value_or(std::string{}) == "boots");
    text = set_status(text, "playable", "Plays \"fine\".");
    t = toml::parse(text);
    REQUIRE(t);
    CHECK(t.table()["status"]["level"].value_or(std::string{}) == "playable");
    CHECK(t.table()["status"]["note"].value_or(std::string{}) == "Plays \"fine\".");
    CHECK(text.find("[status]") == text.rfind("[status]"));  // only one
    // Relocations added after a status keep it valid and in one piece.
    const MergeResult r = merge_suggestions(text, kSuggested);
    REQUIRE(r.error.empty());
    REQUIRE(toml::parse(set_status(r.text, "boots", "x")));
}

TEST_CASE("config: the disc image is pointed at the game's own disc directory") {
    const std::string config =
        "[game]\nid = \"demo\"\n\n[disc]\nimage = \"../../../GDI/Demo (PAL)[!]/Demo.gdi\"\n"
        "\n[binary]\npath = \"extracted/fs/1ST_READ.BIN\"\n";
    const std::string text = set_disc_image(config, "disc/demo.gdi");
    const toml::parse_result t = toml::parse(text);
    REQUIRE(t);
    CHECK(t.table()["disc"]["image"].value_or(std::string{}) == "disc/demo.gdi");
    CHECK(t.table()["binary"]["path"].value_or(std::string{}) == "extracted/fs/1ST_READ.BIN");
    CHECK(set_disc_image("[game]\nid = \"demo\"\n", "disc/demo.gdi") == "[game]\nid = \"demo\"\n");
}

TEST_CASE("config: a text that does not parse is left as it is") {
    const MergeResult r = merge_suggestions("[game\nid = ", kSuggested);
    CHECK(!r.error.empty());
    CHECK(r.text == "[game\nid = ");
}
