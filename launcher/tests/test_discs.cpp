// Finding the dumps in the player's folder, and which of them the library has already.
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../discs.h"
#include "doctest.h"

namespace fs = std::filesystem;
using namespace dream::launcher;

namespace {

struct Scratch {
    fs::path root = fs::temp_directory_path() / "dream_discs_test";
    Scratch() {
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

Game game(const fs::path& root, const std::string& id) {
    Game g;
    g.id = id;
    g.title = "TITLE OF " + id;
    g.config = root / "games" / id / (id + ".toml");
    return g;
}

}  // namespace

TEST_CASE("discs: dumps in the folder and one level down, matched to the game linking them") {
    Scratch s;
    const fs::path dumps = s.root / "GDI";
    write(dumps / "Added (PAL)/Added (PAL).gdi",
          "2\n1 0 4 2352 track01.bin 0\n2 600 0 2352 track02.raw 0\n");
    write(dumps / "Added (PAL)/track01.bin", "data of the added one");
    write(dumps / "Added (PAL)/track02.raw", "audio");
    write(dumps / "New (USA)/New (USA).gdi", "1\n1 0 4 2352 \"track 01.bin\" 0\n");
    write(dumps / "New (USA)/track 01.bin", "data of the added one");  // same size, another file
    write(dumps / "Single.chd", "compressed");
    write(dumps / "notes.txt", "not a disc");
    // The added game's disc/: a copy of the .gdi and hard links to its tracks.
    const fs::path disc = s.root / "games/added/disc";
    write(disc / "added.gdi", "copy");
    fs::create_hard_link(dumps / "Added (PAL)/track01.bin", disc / "track01.bin");
    fs::create_directories(s.root / "games/single/disc");
    fs::create_hard_link(dumps / "Single.chd", s.root / "games/single/disc/single.chd");

    const std::vector<Disc> d =
        find_discs(dumps, {game(s.root, "added"), game(s.root, "other"), game(s.root, "single")});
    REQUIRE(d.size() == 3);
    CHECK(d[0].name == "Added (PAL)");
    CHECK(d[0].game_id == "added");
    CHECK(d[0].game_title == "TITLE OF added");
    CHECK(d[1].name == "New (USA)");
    CHECK(d[1].game_id.empty());  // same size as a linked file, but not the same file
    CHECK(d[2].name == "Single");
    CHECK(d[2].game_id == "single");
    CHECK(find_discs(s.root / "missing", {}).empty());
}

TEST_CASE("discs: a .gdi's track names, quoted or not; a name with a directory is refused") {
    CHECK(gdi_tracks("2\n1 0 4 2352 track01.bin 0\n2 600 0 2352 \"track 02.raw\" 0\n") ==
          std::vector<std::string>{"track01.bin", "track 02.raw"});
    CHECK(gdi_tracks("1\n1 0 4 2352 ../escape.bin 0\n").empty());
}
