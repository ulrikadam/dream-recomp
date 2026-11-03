// The "add a game" sequence, with stand-ins for dcdisc, CMake and the game.
#include <filesystem>
#include <fstream>
#include <string>
#include <toml.hpp>
#include <vector>

#include "../importer.h"
#include "doctest.h"

namespace fs = std::filesystem;
using namespace dream::launcher;

namespace {

struct Scratch {
    fs::path root = fs::temp_directory_path() / "dream_importer_test";
    Scratch() {
        fs::remove_all(root);
        fs::create_directories(root / "games");
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

bool has(const std::vector<std::string>& args, const std::string& word) {
    for (const std::string& a : args)
        if (a == word)
            return true;
    return false;
}

// dcdisc writes the config; CMake succeeds; the game runs its frames and suggests a function the
// first time and nothing the second.
struct FakeTools {
    fs::path root;
    bool wince = false, exists = false;
    std::string product = "MK-99999";
    int test_runs = 0, builds = 0, new_games = 0;
    int operator()(const std::vector<std::string>& args, const fs::path&, const fs::path& log,
                   unsigned) {
        if (has(args, "info")) {
            write(log, "Title:      DEMO\nProduct:    " + product +
                           " V1.000 (20000101)\nRegions:    Europe\nWinCE:      " +
                           (wince ? "YES" : "no") + "\n");
            return 0;
        }
        if (has(args, "new-game")) {
            ++new_games;
            if (wince) {
                write(log, "error: this is a Windows CE title, which is out of scope\n");
                return 1;
            }
            if (exists) {
                write(log, "error: games\\demo\\demo.toml exists (use --force to overwrite)\n");
                return 2;
            }
            write(root / "games/demo/demo.toml",
                  "[game]\nid = \"demo\"\ntitle = \"Demo\"\n\n[disc]\nimage = "
                  "\"../../dumps/Demo (PAL)/Demo (PAL).gdi\"\n# [functions]\n# extra = []\n");
            write(log, "wrote games\\demo\\demo.toml\nwrote games\\demo\\CMakeLists.txt\n");
            return 0;
        }
        if (has(args, "--build")) {
            ++builds;
            write(log, "built\n");
            return 0;
        }
        if (has(args, "--suggest-config")) {
            ++test_runs;
            const fs::path suggest = args.back();
            write(suggest,
                  test_runs == 1 ? "[functions]\nextra = [0x0C0244A0]\n" : "# Nothing to add.\n");
            write(log,
                  "PVR: 1 TA chunks, 2 list inits, 2104 renders (2104 done)\n"
                  "stop: frame limit\n");
            return 0;
        }
        write(log, "configured\n");
        return 0;  // cmake BUILD
    }
};

}  // namespace

// A two-track dump where an owner keeps it, outside the checkout's games/.
static fs::path write_dump(const fs::path& root) {
    const fs::path dir = root / "dumps/Demo (PAL)";
    write(dir / "track01.bin", "data");
    write(dir / "track 02.raw", "audio");
    write(dir / "Demo (PAL).gdi", "2\n1 0 4 2352 track01.bin 0\n2 600 0 2352 \"track 02.raw\" 0\n");
    return dir / "Demo (PAL).gdi";
}

TEST_CASE("import: config, build, run, fold in the suggestion, build again, status") {
    Scratch s;
    FakeTools tools{s.root};
    ImportOptions o;
    o.disc = write_dump(s.root);
    o.root = s.root;
    o.build = s.root / "build";
    o.dcdisc = "dcdisc";
    o.logs = s.root / "logs";
    ImportStatus st;
    import_game(o, std::ref(tools), st);
    CHECK(st.done);
    CHECK(st.ok);
    CHECK(st.id == "demo");
    CHECK(tools.builds == 2);     // once, then again with the suggested function
    CHECK(tools.test_runs == 2);  // the second run suggested nothing: done
    const toml::parse_result t = toml::parse_file((s.root / "games/demo/demo.toml").string());
    REQUIRE(t);
    CHECK(t.table()["functions"]["extra"].as_array()->size() == 1);
    CHECK(t.table()["status"]["level"].value_or(std::string{}) == "boots");
    // The dump has a place in the game's disc/, so the configuration names no owner's folder.
    CHECK(t.table()["disc"]["image"].value_or(std::string{}) == "disc/demo.gdi");
    CHECK(fs::is_regular_file(s.root / "games/demo/disc/demo.gdi"));
    CHECK(fs::file_size(s.root / "games/demo/disc/track 02.raw") == 5);
}

TEST_CASE("import: a .chd is linked under the game's name; a .gdi naming a directory is refused") {
    Scratch s;
    write(s.root / "dumps/Demo.chd", "compressed");
    CHECK(adopt_disc(s.root / "games/demo", "demo", s.root / "dumps/Demo.chd") == "disc/demo.chd");
    CHECK(fs::file_size(s.root / "games/demo/disc/demo.chd") == 10);
    write(s.root / "dumps/bad.gdi", "1\n1 0 4 2352 ../escape.bin 0\n");
    CHECK(adopt_disc(s.root / "games/bad", "bad", s.root / "dumps/bad.gdi").empty());
    CHECK(adopt_disc(s.root / "games/none", "none", s.root / "dumps/missing.gdi").empty());
}

TEST_CASE("import: a game already there is built and tested again, its own status kept") {
    Scratch s;
    FakeTools tools{s.root};
    tools.exists = true;
    write(s.root / "games/demo/demo.toml",
          "[game]\nid = \"demo\"\n\n[status]\nlevel = \"playable\"\nnote = \"Finished a race.\"\n");
    ImportOptions o;
    o.disc = s.root / "demo.gdi";
    o.root = s.root;
    o.build = s.root / "build";
    o.dcdisc = "dcdisc";
    o.logs = s.root / "logs";
    ImportStatus st;
    import_game(o, std::ref(tools), st);
    CHECK(st.ok);
    CHECK(st.id == "demo");
    CHECK(tools.builds == 2);
    const toml::parse_result t = toml::parse_file((s.root / "games/demo/demo.toml").string());
    REQUIRE(t);
    CHECK(t.table()["status"]["level"].value_or(std::string{}) == "playable");
    CHECK(t.table()["functions"]["extra"].as_array()->size() == 1);
}

TEST_CASE("import: cancelled, it stops after the program that was running") {
    Scratch s;
    ImportStatus st;
    int runs = 0;
    auto tools = [&](const std::vector<std::string>&, const fs::path&, const fs::path&, unsigned) {
        ++runs;
        st.cancel = true;  // as the window does while dcdisc reads the disc
        return -1;
    };
    ImportOptions o;
    o.disc = s.root / "demo.gdi";
    o.root = s.root;
    o.build = s.root / "build";
    o.logs = s.root / "logs";
    import_game(o, tools, st);
    CHECK(st.done);
    CHECK(!st.ok);
    CHECK(st.message == "AJOUT ANNULE");
    CHECK(runs == 1);
}

TEST_CASE("import: what the last round found is built in as well") {
    Scratch s;
    int builds = 0, runs = 0;
    auto tools = [&](const std::vector<std::string>& args, const fs::path&, const fs::path& log,
                     unsigned) {
        if (has(args, "new-game")) {
            write(s.root / "games/demo/demo.toml", "[game]\nid = \"demo\"\ntitle = \"Demo\"\n");
            write(log, "wrote games/demo/demo.toml\n");
        } else if (has(args, "--build")) {
            ++builds;
        } else if (has(args, "--suggest-config")) {
            ++runs;  // every run finds one more function
            write(args.back(),
                  "[functions]\nextra = [0x0C0100" + std::to_string(10 + runs) + "]\n");
            write(log, "2000 renders\nstop: frame limit\n");
        }
        return 0;
    };
    ImportOptions o;
    o.disc = s.root / "demo.gdi";
    o.root = s.root;
    o.build = s.root / "build";
    o.logs = s.root / "logs";
    ImportStatus st;
    import_game(o, tools, st);
    CHECK(st.ok);
    CHECK(runs == 3);
    CHECK(builds == 4);  // three rounds, then once more for the third round's function
}

TEST_CASE("play: a session's findings go into the config once") {
    Scratch s;
    const fs::path config = s.root / "games/demo/demo.toml";
    const fs::path suggest = s.root / "logs/demo-play-suggest.toml";
    write(config, "[game]\nid = \"demo\"\ntitle = \"Demo\"\n");
    CHECK(fold_findings(config, suggest).functions == 0);  // no file: the game wrote nothing
    write(suggest,
          "[functions]\nextra = [0x0C0B2D32,  # called 2 times\n"
          "         0x0C0B2D3C]  # called 2 times\n");
    const Findings first = fold_findings(config, suggest);
    CHECK(first.error.empty());
    CHECK(first.functions == 2);
    const Findings again = fold_findings(config, suggest);
    CHECK(again.functions == 0);
    const toml::parse_result t = toml::parse_file(config.string());
    REQUIRE(t);
    CHECK(t.table()["functions"]["extra"].as_array()->size() == 2);
}

TEST_CASE("play: the game is built again, or later when that is stopped") {
    Scratch s;
    ImportOptions o;
    o.root = s.root;
    o.build = s.root / "build";
    o.logs = s.root / "logs";
    std::vector<std::string> built;
    auto ok = [&](const std::vector<std::string>& args, const fs::path&, const fs::path&,
                  unsigned) {
        built = args;
        return 0;
    };
    ImportStatus st;
    update_game(o, "demo", "DEMO AMELIORE", ok, st);
    CHECK(st.ok);
    CHECK(st.message == "DEMO AMELIORE");
    CHECK(has(built, "demo_boot"));
    ImportStatus stopped;
    auto cancel = [&](const std::vector<std::string>&, const fs::path&, const fs::path&, unsigned) {
        stopped.cancel = true;
        return -1;
    };
    update_game(o, "demo", "DEMO AMELIORE", cancel, stopped);
    CHECK(!stopped.ok);
    CHECK(stopped.message.find("REPORTEE") != std::string::npos);
}

TEST_CASE("import: a Windows CE disc stops at once with a clear message") {
    Scratch s;
    FakeTools tools{s.root};
    tools.wince = true;
    ImportOptions o;
    o.disc = s.root / "rally.gdi";
    o.root = s.root;
    o.build = s.root / "build";
    o.dcdisc = "dcdisc";
    o.logs = s.root / "logs";
    ImportStatus st;
    import_game(o, std::ref(tools), st);
    CHECK(st.done);
    CHECK(!st.ok);
    CHECK(st.message.find("WINDOWS CE") != std::string::npos);
    CHECK(tools.new_games == 0);  // refused before extracting anything
    CHECK(tools.builds == 0);
}

TEST_CASE("import: a disc the library knows by its number and region is not added twice") {
    Scratch s;
    FakeTools tools{s.root};
    tools.product = "T8118D  50";
    write(
        s.root / "games/f355/f355.toml",
        "[game]\nid = \"f355\"\ntitle = \"F355\"\nregion = \"Europe\"\nproduct = \"T8118D  50\"\n");
    ImportOptions o;
    o.disc = s.root / "F355 Challenge (PAL).gdi";
    o.root = s.root;
    o.build = s.root / "build";
    o.dcdisc = "dcdisc";
    o.logs = s.root / "logs";
    ImportStatus st;
    import_game(o, std::ref(tools), st);
    CHECK(st.id == "f355");
    CHECK(tools.new_games == 0);
    CHECK(tools.builds >= 1);
}
