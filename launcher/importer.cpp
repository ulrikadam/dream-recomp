#include "importer.h"

#include <cctype>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <system_error>
#include <toml.hpp>

#include "config_edit.h"
#include "discs.h"
#include "library.h"

namespace dream::launcher {

namespace fs = std::filesystem;

const char* const kImportPresses = "start@600,start@900,start@1200,a@1500,a@1800,a@2100,a@2400";
const char* const kLauncherNote = "Added by the launcher";

namespace {

std::string utf8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool write_file(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// The game id dcdisc chose, from its "wrote games/<id>/<id>.toml" line.
std::string written_id(const std::string& output) {
    static const std::regex kWrote(R"(wrote .*[\\/]([^\\/\s]+)\.toml)");
    std::smatch m;
    return std::regex_search(output, m, kWrote) ? m[1].str() : std::string{};
}

// The id of a game already in the library, from "error: games/<id>/<id>.toml exists".
std::string existing_id(const std::string& output) {
    static const std::regex kExists(R"(([^\\/\s]+)\.toml exists)");
    std::smatch m;
    return std::regex_search(output, m, kExists) ? m[1].str() : std::string{};
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    const std::size_t start = s.find_first_not_of(" \t");
    return start == std::string::npos ? std::string{} : s.substr(start);
}

// What `dcdisc info` says about the disc: enough to know it again and to refuse Windows CE before
// the long extraction.
struct DiscInfo {
    std::string product, region;
    bool wince = false;
};

DiscInfo read_info(const std::string& out) {
    DiscInfo d;
    std::smatch m;
    static const std::regex kProduct(R"(Product:[ \t]+(.*?)[ \t]+V\d)");
    if (std::regex_search(out, m, kProduct))
        d.product = trim(m[1].str());
    static const std::regex kRegions(R"(Regions:[ \t]+([^,\r\n]*))");
    if (std::regex_search(out, m, kRegions))
        d.region = trim(m[1].str());
    static const std::regex kWince(R"(WinCE:[ \t]+YES)");
    d.wince = std::regex_search(out, kWince);
    return d;
}

// The library's game for this disc: the same product number for the same region. Crazy Taxi's
// American and European discs share their number.
std::string known_id(const fs::path& games, const fs::path& build, const DiscInfo& d) {
    if (d.product.empty())
        return {};
    for (const Game& g : scan(games, build))
        if (g.product == d.product && g.region == d.region)
            return g.id;
    return {};
}

// No status yet, or one the launcher wrote: a person's own is left alone.
bool status_is_ours(const std::string& config) {
    const toml::parse_result t = toml::parse(config);
    if (!t)
        return false;
    if (!t.table()["status"])
        return true;
    const std::string note = t.table()["status"]["note"].value_or(std::string{});
    return note.rfind(kLauncherNote, 0) == 0;
}

struct RunOutcome {
    bool reached_limit = false;  // "stop: frame limit": it ran every frame asked for
    unsigned long renders = 0;
    std::string fault;  // the "fault: ..." line, when it stopped on one
};

RunOutcome read_outcome(const std::string& log) {
    RunOutcome o;
    o.reached_limit = log.find("stop: frame limit") != std::string::npos;
    static const std::regex kRenders(R"((\d+) renders)");
    std::smatch m;
    if (std::regex_search(log, m, kRenders))
        o.renders = std::stoul(m[1].str());
    static const std::regex kFault(R"(fault: ([^\r\n(]*))");
    if (std::regex_search(log, m, kFault))
        o.fault = m[1].str();
    while (!o.fault.empty() && o.fault.back() == ' ') o.fault.pop_back();
    return o;
}

void set_step(ImportStatus& status, const std::string& s) {
    std::lock_guard<std::mutex> lock(status.mutex);
    status.step = s;
}

void set_done(ImportStatus& status, bool ok, const std::string& message, const fs::path& log) {
    std::lock_guard<std::mutex> lock(status.mutex);
    status.done = true;
    status.ok = ok;
    status.message = message;
    status.log = log;
    status.step.clear();
}

int build_game(const ImportOptions& opt, const std::string& id, const Runner& run) {
    return run({opt.cmake, "--build", utf8(opt.build), "--target", id + "_boot"}, opt.root,
               opt.logs / (id + "-build.log"), 3600);
}

// `to` as a hard link to `from`; an existing `to` of the same size counts as done.
bool link_file(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (fs::exists(to, ec))
        return fs::file_size(to, ec) == fs::file_size(from, ec) && !ec;
    fs::create_hard_link(from, to, ec);
    return !ec;
}

}  // namespace

std::string adopt_disc(const fs::path& game_dir, const std::string& id, const fs::path& image) {
    std::error_code ec;
    if (!fs::is_regular_file(image, ec))
        return {};
    const fs::path disc = game_dir / "disc";
    fs::create_directories(disc, ec);
    std::string ext = image.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string name = id + ext;
    if (ext == ".gdi") {
        // The tracks keep their names, next to a copy of the .gdi that lists them.
        const std::string text = read_file(image);
        const std::vector<std::string> tracks = gdi_tracks(text);
        if (tracks.empty())
            return {};
        for (const std::string& t : tracks)
            if (!link_file(image.parent_path() / t, disc / t))
                return {};
        if (!write_file(disc / name, text))
            return {};
    } else if (!link_file(image, disc / name)) {
        return {};
    }
    return "disc/" + name;
}

Findings fold_findings(const fs::path& config, const fs::path& suggest) {
    Findings f;
    std::error_code ec;
    if (!fs::is_regular_file(suggest, ec))
        return f;  // the game stopped before writing it
    const MergeResult merged = merge_suggestions(read_file(config), read_file(suggest));
    f.error = merged.error;
    f.functions = merged.functions;
    f.relocations = merged.relocations;
    if (f.error.empty() && f.functions + f.relocations > 0 && !write_file(config, merged.text))
        f.error = "cannot write " + utf8(config);
    return f;
}

void update_game(const ImportOptions& opt, const std::string& id, const std::string& done,
                 const Runner& run, ImportStatus& status) {
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        status.id = id;
    }
    std::error_code ec;
    fs::create_directories(opt.logs, ec);
    set_step(status, "TRADUCTION ET COMPILATION");
    const int built = build_game(opt, id, run);
    if (status.cancel)
        return set_done(status, false,
                        "AMELIORATION REPORTEE: ELLE SE FERA APRES LA PROCHAINE PARTIE", {});
    if (built != 0)
        return set_done(status, false, "COMPILATION IMPOSSIBLE: VOIR " + upper(id) + "-BUILD.LOG",
                        opt.logs / (id + "-build.log"));
    set_done(status, true, done, {});
}

void import_game(const ImportOptions& opt, const Runner& run, ImportStatus& status) {
    auto step = [&](const std::string& s) { set_step(status, s); };
    auto finish = [&](bool ok, const std::string& message, const fs::path& log = {}) {
        set_done(status, ok, message, log);
    };
    auto cancelled = [&] {
        if (!status.cancel)
            return false;
        finish(false, "AJOUT ANNULE");
        return true;
    };
    std::error_code ec;
    fs::create_directories(opt.logs, ec);

    // 1. The configuration, from the disc, unless the library has this disc already: then it is
    //    built and tested again rather than refused or added twice.
    step("LECTURE DU DISQUE");
    const fs::path info_log = opt.logs / "import-info.log";
    run({utf8(opt.dcdisc), "info", utf8(opt.disc)}, opt.root, info_log, 300);
    if (cancelled())
        return;
    const DiscInfo info = read_info(read_file(info_log));
    if (info.wince)
        return finish(false, "WINDOWS CE: CE JEU N EST PAS PRIS EN CHARGE", info_log);
    std::string id = known_id(opt.root / "games", opt.build, info);
    if (id.empty()) {
        step("EXTRACTION DU DISQUE");
        const fs::path dcdisc_log = opt.logs / "import-dcdisc.log";
        const int read = run({utf8(opt.dcdisc), "new-game", utf8(opt.disc), "--dir",
                              utf8(opt.root / "games"), "--brief"},
                             opt.root, dcdisc_log, 900);
        if (cancelled())
            return;
        const std::string said = read_file(dcdisc_log);
        if (said.find("Windows CE") != std::string::npos)
            return finish(false, "WINDOWS CE: CE JEU N EST PAS PRIS EN CHARGE", dcdisc_log);
        id = existing_id(said);  // a configuration of that name is there already
        const bool written = id.empty() && read == 0;
        if (written)
            id = written_id(said);
        if (id.empty())
            return finish(false, "LECTURE DU DISQUE IMPOSSIBLE: VOIR IMPORT-DCDISC.LOG",
                          dcdisc_log);
        // dcdisc names the image where it is; the game's disc/ makes the file shareable.
        const fs::path toml = opt.root / "games" / id / (id + ".toml");
        if (written)
            if (const std::string image = adopt_disc(toml.parent_path(), id, opt.disc);
                !image.empty())
                write_file(toml, set_disc_image(read_file(toml), image));
    }
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        status.id = id;
    }
    const fs::path config = opt.root / "games" / id / (id + ".toml");

    // 2. CMake picks the new game directory up.
    step("CONFIGURATION DU PROJET");
    const fs::path cmake_log = opt.logs / "import-cmake.log";
    const int configured = run({opt.cmake, utf8(opt.build)}, opt.root, cmake_log, 900);
    if (cancelled())
        return;
    if (configured != 0)
        return finish(false, "CONFIGURATION IMPOSSIBLE: OUTILS DE COMPILATION?", cmake_log);

    // 3. Build, run, fold in what the run found, again until it finds nothing new.
    const fs::path exe = opt.build / "games" / id / boot_executable(id);
    const fs::path build_log = opt.logs / (id + "-build.log");
    const fs::path test_log = opt.logs / (id + "-test.log");
    const fs::path suggest = opt.logs / (id + "-suggest.toml");
    RunOutcome outcome;
    bool stale = false;  // the last round changed the configuration after its build
    for (unsigned round = 1; round <= opt.rounds; ++round) {
        const std::string of =
            " (" + std::to_string(round) + "/" + std::to_string(opt.rounds) + ")";
        step("TRADUCTION ET COMPILATION" + of);
        const int built = build_game(opt, id, run);
        if (cancelled())
            return;
        if (built != 0)
            return finish(false, "COMPILATION IMPOSSIBLE: VOIR " + upper(id) + "-BUILD.LOG",
                          build_log);
        stale = false;
        step("ESSAI SANS FENETRE" + of);
        fs::remove(suggest, ec);
        run({utf8(exe), "--config", utf8(config), "--no-audio", "--rtc-seed", "1", "--max-frames",
             std::to_string(opt.frames), "--press", kImportPresses, "--suggest-config",
             utf8(suggest)},
            opt.root, test_log, 900);
        if (cancelled())
            return;
        outcome = read_outcome(read_file(test_log));
        const MergeResult merged = merge_suggestions(read_file(config), read_file(suggest));
        if (!merged.error.empty() || merged.functions + merged.relocations == 0)
            break;
        step("AJOUT DES REGLAGES TROUVES" + of);
        write_file(config, merged.text);
        stale = true;
    }
    // The last round's findings go into the executable too, without another test run.
    if (stale) {
        step("TRADUCTION ET COMPILATION (FIN)");
        const int built = build_game(opt, id, run);
        if (cancelled())
            return;
        if (built != 0)
            return finish(false, "COMPILATION IMPOSSIBLE: VOIR " + upper(id) + "-BUILD.LOG",
                          build_log);
    }

    // 4. What the library says about it.
    std::string level, note, message;
    if (outcome.reached_limit && outcome.renders > 0) {
        level = "boots";
        note = std::string(kLauncherNote) + ": ran " + std::to_string(opt.frames) +
               " frames headless and drew " + std::to_string(outcome.renders) +
               " of them; not played yet.";
        message = "PRET A ESSAYER: " + upper(id);
    } else if (!outcome.fault.empty()) {
        level = "nothing";
        note = std::string(kLauncherNote) + ": stops at " + outcome.fault + ".";
        message = "AJOUTE, MAIS IL S ARRETE AU DEMARRAGE: " + upper(id);
    } else {
        level = "nothing";
        note = std::string(kLauncherNote) + ": draws nothing in " + std::to_string(opt.frames) +
               " frames.";
        message = "AJOUTE, MAIS IL N AFFICHE RIEN: " + upper(id);
    }
    const std::string text = read_file(config);
    if (status_is_ours(text))
        write_file(config, set_status(text, level, note));
    finish(level == "boots", message, test_log);
}

}  // namespace dream::launcher
