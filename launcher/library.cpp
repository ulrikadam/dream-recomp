#include "library.h"

#include <algorithm>
#include <system_error>
#include <toml.hpp>

namespace dream::launcher {

namespace fs = std::filesystem;

Level parse_level(std::string_view text) {
    if (text == "playable")
        return Level::Playable;
    if (text == "ingame")
        return Level::InGame;
    if (text == "boots")
        return Level::Boots;
    if (text == "nothing")
        return Level::Nothing;
    if (text == "unsupported")
        return Level::Unsupported;
    return Level::Unknown;
}

std::string_view level_label(Level level) {
    switch (level) {
        case Level::Playable:
            return "JOUABLE";
        case Level::InGame:
            return "EN JEU";
        case Level::Boots:
            return "DEMARRE";
        case Level::Nothing:
            return "NE DEMARRE PAS";
        case Level::Unsupported:
            return "NON PRIS EN CHARGE";
        case Level::Unknown:
            break;
    }
    return "NON TESTE";
}

std::string boot_executable(std::string_view id) {
#ifdef _WIN32
    return std::string(id) + "_boot.exe";
#else
    return std::string(id) + "_boot";
#endif
}

std::string wince_executable() {
#ifdef _WIN32
    return "dream-wince.exe";
#else
    return "dream-wince";
#endif
}

std::optional<Game> read_game(const fs::path& config, const fs::path& build_dir,
                              std::string* error) {
    toml::parse_result r = toml::parse_file(config.string());
    if (!r) {
        if (error)
            *error = config.string() + ": " + std::string(r.error().description());
        return std::nullopt;
    }
    const toml::table& t = r.table();
    Game g;
    g.id = t["game"]["id"].value_or(std::string{});
    g.title = t["game"]["title"].value_or(std::string{});
    if (g.id.empty() || g.title.empty()) {
        if (error)
            *error = config.string() + ": game.id and game.title are required";
        return std::nullopt;
    }
    g.region = t["game"]["region"].value_or(std::string{});
    g.product = t["game"]["product"].value_or(std::string{});
    g.level = parse_level(t["status"]["level"].value_or(std::string{}));
    g.note = t["status"]["note"].value_or(std::string{});
    g.config = config;
    g.exe = build_dir / "games" / g.id / boot_executable(g.id);
    std::error_code ec;
    if (const std::string wince = t["wince"]["executable"].value_or(std::string{});
        !wince.empty()) {
        g.wince = config.parent_path() / wince;
        g.exe = build_dir / "tools" / "wince" / wince_executable();
    }
    g.built =
        fs::is_regular_file(g.exe, ec) && (g.wince.empty() || fs::is_regular_file(g.wince, ec));
    if (const std::string image = t["disc"]["image"].value_or(std::string{}); !image.empty())
        g.image = config.parent_path() / image;
    g.disc = !g.image.empty() && fs::is_regular_file(g.image, ec);
    return g;
}

std::vector<Game> scan(const fs::path& games_dir, const fs::path& build_dir, std::string* errors) {
    std::vector<Game> games;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(games_dir, ec)) {
        if (!e.is_directory(ec))
            continue;
        const fs::path config = e.path() / (e.path().filename().string() + ".toml");
        if (!fs::is_regular_file(config, ec))
            continue;
        std::string error;
        if (std::optional<Game> g = read_game(config, build_dir, &error))
            games.push_back(std::move(*g));
        else if (errors)
            *errors += error + "\n";
    }
    std::sort(games.begin(), games.end(),
              [](const Game& a, const Game& b) { return a.title < b.title; });
    return games;
}

}  // namespace dream::launcher
