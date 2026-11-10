// The launcher's game library: status levels, and reading games from their configuration files.
#include <filesystem>
#include <fstream>
#include <string>

#include "../library.h"
#include "doctest.h"

namespace fs = std::filesystem;
using namespace dream::launcher;

namespace {

// A scratch games/ and build/ tree, removed when the test ends.
struct Tree {
    fs::path root = fs::temp_directory_path() / "dream_launcher_test";
    Tree() {
        fs::remove_all(root);
        fs::create_directories(root / "games");
        fs::create_directories(root / "build");
    }
    ~Tree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    void write(const fs::path& rel, const std::string& text) const {
        fs::create_directories((root / rel).parent_path());
        std::ofstream(root / rel, std::ios::binary) << text;
    }
};

}  // namespace

TEST_CASE("status levels parse from their config names") {
    CHECK(parse_level("playable") == Level::Playable);
    CHECK(parse_level("ingame") == Level::InGame);
    CHECK(parse_level("boots") == Level::Boots);
    CHECK(parse_level("nothing") == Level::Nothing);
    CHECK(parse_level("unsupported") == Level::Unsupported);
    CHECK(parse_level("") == Level::Unknown);
    CHECK(parse_level("Playable") == Level::Unknown);
    CHECK(level_label(Level::Unknown) == "NON TESTE");
}

TEST_CASE("a game reads its title, status, executable and disc") {
    Tree t;
    t.write("games/alpha/alpha.toml",
            "[game]\nid = \"alpha\"\ntitle = \"Alpha\"\n"
            "product = \"MK-1\"\n[disc]\nimage = \"disc/a.gdi\"\n"
            "[status]\nlevel = \"playable\"\nnote = \"All good.\"\n");
    t.write("games/alpha/disc/a.gdi", "1\n");
    t.write("build/games/alpha/" + boot_executable("alpha"), "");
    std::string error;
    const auto g = read_game(t.root / "games/alpha/alpha.toml", t.root / "build", &error);
    REQUIRE_MESSAGE(g, error);
    CHECK(g->id == "alpha");
    CHECK(g->title == "Alpha");
    CHECK(g->product == "MK-1");
    CHECK(g->level == Level::Playable);
    CHECK(g->note == "All good.");
    CHECK(g->built);
    CHECK(g->disc);
}

TEST_CASE("a Windows CE title runs on dream-wince, with the executable on its disc") {
    Tree t;
    t.write("games/rally/rally.toml",
            "[game]\nid = \"rally\"\ntitle = \"Rally\"\n[disc]\nimage = \"disc/r.gdi\"\n"
            "[wince]\nexecutable = \"extracted/fs/RALLY.EXE\"\n");
    t.write("games/rally/disc/r.gdi", "1\n");
    t.write("build/games/rally/" + boot_executable("rally"), "");  // not what runs it
    std::string error;
    auto g = read_game(t.root / "games/rally/rally.toml", t.root / "build", &error);
    REQUIRE_MESSAGE(g, error);
    CHECK(g->exe == t.root / "build/tools/wince" / wince_executable());
    CHECK(g->wince == t.root / "games/rally/extracted/fs/RALLY.EXE");
    CHECK(g->image == t.root / "games/rally/disc/r.gdi");
    CHECK(g->disc);
    CHECK_FALSE(g->built);  // neither dream-wince nor the title's executable yet
    t.write("build/tools/wince/" + wince_executable(), "");
    CHECK_FALSE(read_game(t.root / "games/rally/rally.toml", t.root / "build")->built);
    t.write("games/rally/extracted/fs/RALLY.EXE", "");
    CHECK(read_game(t.root / "games/rally/rally.toml", t.root / "build")->built);
}

TEST_CASE("scan skips directories without a config and sorts by title") {
    Tree t;
    t.write("games/zeta/zeta.toml", "[game]\nid = \"zeta\"\ntitle = \"Beta\"\n");
    t.write("games/alpha/alpha.toml", "[game]\nid = \"alpha\"\ntitle = \"Alpha\"\n");
    t.write("games/notes/readme.txt", "not a game");
    t.write("games/broken/broken.toml", "[game]\nid = \"broken\"\n");
    std::string errors;
    const auto games = scan(t.root / "games", t.root / "build", &errors);
    REQUIRE(games.size() == 2);
    CHECK(games[0].title == "Alpha");
    CHECK(games[1].title == "Beta");
    CHECK(games[1].level == Level::Unknown);
    CHECK_FALSE(games[1].built);
    CHECK_FALSE(games[1].disc);
    CHECK(errors.find("broken") != std::string::npos);
}
