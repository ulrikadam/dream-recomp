// dream_launcher: the game library window (WP3.6).
//
// Lists the games under games/, with how far each one gets, whether it is built and whether its
// disc is there, and starts one with its memory cards. The settings screen is the games' own
// (controller and memory card tabs), opened in this window: bindings are saved to the file every
// game reads, and the cards chosen for a game are the ones it is started with.
//
// The window is the games' window too (vk::Window), so the keys, pads, binding capture and
// device list behave the same here as in a game. The library is drawn with the overlay font
// into an RGBA buffer and handed to the presenter, as a game's frame is.
//
// AJOUTER UN JEU asks for a disc image and does what a person does by hand to bring a title up
// (importer.h): dcdisc, CMake, test runs that find what discovery missed, and a status. It runs on
// a thread of its own with the steps shown over the library; B stops it. Every session played
// from here does the same in small: what the game reached that its translation lacks goes into
// its configuration, and the game is built again behind the library (after_play).
//
//   dream_launcher [--root DIR] [--build DIR] [--saves DIR] [--logs DIR] [--ui-assets DIR]
//                  [--dcdisc PROGRAM] [--discs DIR]
//   dream_launcher --import DISC     adds a game without a window, printing each step
//   dream_launcher --screenshot FILE.ppm [--select N] [--tab controller|memory|import|update]
//       draws one frame to a file, no window; --tab draws the settings screen over it, or the
//       screen shown while a game is added or built again
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "dream/render/overlay.h"
#include "dream/render/settings_menu.h"
#include "dream/render/vk/present.h"
#include "dream/render/vk/window.h"
#include "dream/runtime/maple/maple.h"
#include "dream/runtime/maple/vmu_fs.h"

#include "importer.h"
#include "library.h"
#include <SDL3/SDL.h>

namespace {

namespace fs = std::filesystem;
namespace rd = dream::render;
using dream::launcher::Game;
using dream::launcher::Level;
using Control = rd::vk::Control;
using Menu = rd::SettingsMenu;

constexpr int kW = 960, kH = 540;

constexpr std::uint32_t rgb(unsigned r, unsigned g, unsigned b) {
    return 0xFF000000u | b << 16 | g << 8 | r;  // red in the low byte, as the overlay draws
}
constexpr std::uint32_t kBg = rgb(16, 20, 30), kHeader = rgb(28, 36, 54), kPanel = rgb(24, 30, 44),
                        kSelect = rgb(48, 68, 108), kButton = rgb(40, 50, 72),
                        kFocus = rgb(236, 184, 48), kWhite = rgb(240, 240, 240),
                        kLight = rgb(200, 206, 218), kGrey = rgb(128, 136, 152),
                        kInk = rgb(16, 20, 30), kGood = rgb(80, 200, 110),
                        kWarn = rgb(236, 150, 60);

std::uint32_t level_colour(Level l) {
    switch (l) {
        case Level::Playable:
            return rgb(60, 190, 90);
        case Level::InGame:
            return rgb(220, 190, 50);
        case Level::Boots:
            return rgb(230, 130, 40);
        case Level::Nothing:
            return rgb(210, 60, 60);
        case Level::Unsupported:
            return rgb(110, 110, 120);
        case Level::Unknown:
            break;
    }
    return rgb(90, 110, 140);
}

std::string utf8(const fs::path& p) {
    const auto s = p.u8string();  // std::u8string from C++20 on, std::string before
    return std::string(s.begin(), s.end());
}

fs::path from_utf8(const char* s) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s)));
#else
    return fs::u8path(s);
#endif
}

std::string upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return s;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Options {
    fs::path root, build, saves, logs, ui_assets;
    fs::path dcdisc;       // the disc tool, for adding a game
    fs::path discs;        // where the file dialog opens
    fs::path import_disc;  // --import: add this disc without a window
    std::string screenshot, tab;
    int select = 0;
};

enum Action : int { kPlay, kAdd, kSettings, kQuit, kActions };
constexpr std::string_view kActionLabels[kActions] = {"JOUER", "AJOUTER UN JEU",
                                                      "REGLAGES MANETTE / VMU", "QUITTER"};
constexpr std::string_view kHelp = "HAUT/BAS: JEU  GAUCHE/DROITE: ACTION  A: VALIDER  B: QUITTER";

using Slots = std::array<std::string, Menu::kSlots>;  // card file per slot, "" for an empty slot

struct State {
    std::vector<Game> games;
    int sel = 0, action = kPlay;
    std::string message;
    std::map<std::string, Slots> cards;  // the player's choice per game, by id

    const Game* game() const {
        return games.empty() ? nullptr : &games[static_cast<std::size_t>(sel)];
    }
    // A game's cards: what was chosen for it, else its own card in the first slot.
    Slots& slots(const std::string& id) {
        auto it = cards.find(id);
        if (it == cards.end())
            it = cards.emplace(id, Slots{id + "-vmu.bin", ""}).first;
        return it->second;
    }
};

// The card choices live beside the cards: "id<TAB>slot1<TAB>slot2" per line.
fs::path cards_file(const Options& o) {
    return o.saves / "launcher-cards.txt";
}

void load_cards(State& s, const Options& o) {
    std::ifstream in(cards_file(o));
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t a = line.find('\t'), b = line.find('\t', a + 1);
        if (a == std::string::npos || b == std::string::npos)
            continue;
        s.cards[line.substr(0, a)] = Slots{line.substr(a + 1, b - a - 1), line.substr(b + 1)};
    }
}

void save_cards(const State& s, const Options& o) {
    std::error_code ec;
    fs::create_directories(o.saves, ec);
    std::ofstream out(cards_file(o));
    for (const auto& [id, slots] : s.cards)
        out << id << '\t' << slots[0] << '\t' << slots[1] << '\n';
}

struct Canvas {
    std::vector<std::uint32_t> px = std::vector<std::uint32_t>(std::size_t{kW} * kH);
    void fill(int x, int y, int w, int h, std::uint32_t c) {
        rd::fill_rect(px.data(), kW, kW, kH, x, y, w, h, c);
    }
    void text(int x, int y, std::string_view s, unsigned scale, std::uint32_t fg) {
        rd::TextStyle st;
        st.fg = fg;
        st.bg = 0;
        st.scale = scale;
        rd::draw_text(px.data(), kW, kW, kH, x, y, s, st);
    }
};

int width_of(std::string_view s, unsigned scale) {
    return static_cast<int>(rd::text_width(s, scale));
}

std::vector<std::string> wrap(std::string_view s, int max_px, unsigned scale) {
    std::vector<std::string> lines;
    std::string line, word;
    auto place = [&] {
        if (word.empty())
            return;
        std::string joined = line.empty() ? word : line + " " + word;
        if (!line.empty() && width_of(joined, scale) > max_px) {
            lines.push_back(line);
            line = word;
        } else {
            line = std::move(joined);
        }
        word.clear();
    };
    for (char c : s) {
        if (c == ' ' || c == '\n')
            place();
        else
            word += c;
    }
    place();
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

void draw(Canvas& cv, State& s) {
    cv.fill(0, 0, kW, kH, kBg);
    cv.fill(0, 0, kW, 56, kHeader);
    cv.text(24, 12, "DREAM-RECOMP", 4, kWhite);
    const std::string count = std::to_string(s.games.size()) + " JEUX";
    cv.text(kW - 24 - width_of(count, 2), 22, count, 2, kGrey);

    // The list: a coloured mark for the status, the title grey until the game is built.
    constexpr int lx = 24, ly = 72, lw = 376, rowh = 36, rows = 10;
    cv.fill(lx, ly, lw, rows * rowh, kPanel);
    if (s.games.empty())
        cv.text(lx + 12, ly + 12, "AUCUN JEU DANS GAMES/", 2, kGrey);
    const int first = std::max(0, s.sel - rows + 1);
    for (int i = 0; i < rows && first + i < static_cast<int>(s.games.size()); ++i) {
        const Game& g = s.games[static_cast<std::size_t>(first + i)];
        const int y = ly + i * rowh;
        if (first + i == s.sel)
            cv.fill(lx, y, lw, rowh, kSelect);
        cv.fill(lx + 12, y + 12, 12, 12, level_colour(g.level));
        cv.text(lx + 36, y + 10, rd::fit_text(g.title, lw - 48, 2), 2, g.built ? kWhite : kGrey);
    }

    // The selected game.
    constexpr int dx = 416, dy = ly, dw = kW - 24 - dx, dh = rows * rowh;
    cv.fill(dx, dy, dw, dh, kPanel);
    if (const Game* gp = s.game()) {
        const Game& g = *gp;
        const int x = dx + 16, w = dw - 32;
        int y = dy + 16;
        cv.text(x, y, rd::fit_text(g.title, w, 3), 3, kWhite);
        y += 36;
        std::string sub = g.product;
        if (!g.region.empty())
            sub += (sub.empty() ? "" : "  ") + g.region;
        cv.text(x, y, rd::fit_text(sub, w, 2), 2, kGrey);
        y += 28;
        const std::string_view label = dream::launcher::level_label(g.level);
        cv.fill(x, y, width_of(label, 2) + 16, 28, level_colour(g.level));
        cv.text(x + 8, y + 7, label, 2, kInk);
        y += 44;
        const std::string note = g.note.empty() ? "PAS ENCORE DE NOTE POUR CE JEU." : g.note;
        for (const std::string& l : wrap(note, w, 2)) {
            if (y > dy + dh - 112)
                break;
            cv.text(x, y, l, 2, kLight);
            y += 20;
        }
        y = dy + dh - 88;
        auto line = [&](std::string_view key, std::string_view value, std::uint32_t c) {
            cv.text(x, y, key, 2, kGrey);
            cv.text(x + 156, y, rd::fit_text(value, w - 156, 2), 2, c);
            y += 24;
        };
        line("EXECUTABLE", g.built ? "PRET" : "A COMPILER", g.built ? kGood : kWarn);
        line("DISQUE", g.disc ? "PRESENT" : "ABSENT", g.disc ? kGood : kWarn);
        const Slots& cards = s.slots(g.id);
        line("CARTE", cards[0].empty() ? "AUCUNE" : rd::card_label(cards[0]), kLight);
    }

    int ax = 24;
    for (int a = 0; a < kActions; ++a) {
        const std::string_view label = kActionLabels[a];
        const int w = width_of(label, 2) + 24;
        const bool focus = a == s.action;
        cv.fill(ax, 456, w, 32, focus ? kFocus : kButton);
        cv.text(ax + 12, 465, label, 2, focus ? kInk : kWhite);
        ax += w + 12;
    }
    if (s.message.empty())
        cv.text(24, 508, kHelp, 2, kGrey);
    else
        cv.text(24, 508, rd::fit_text(s.message, kW - 48, 2), 2, kWarn);
}

bool write_ppm(const std::string& path, const Canvas& cv) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    std::fprintf(f, "P6\n%d %d\n255\n", kW, kH);
    std::vector<unsigned char> row;
    for (std::uint32_t p : cv.px) {
        row.push_back(static_cast<unsigned char>(p & 0xFF));
        row.push_back(static_cast<unsigned char>(p >> 8 & 0xFF));
        row.push_back(static_cast<unsigned char>(p >> 16 & 0xFF));
    }
    const bool ok = std::fwrite(row.data(), 1, row.size(), f) == row.size();
    return std::fclose(f) == 0 && ok;
}

void rescan(State& s, const Options& o) {
    const std::string keep = s.game() ? s.game()->id : std::string{};
    std::string errors;
    s.games = dream::launcher::scan(o.root / "games", o.build, &errors);
    if (!errors.empty())
        std::fprintf(stderr, "%s", errors.c_str());
    s.sel = 0;
    for (std::size_t i = 0; i < s.games.size(); ++i)
        if (s.games[i].id == keep)
            s.sel = static_cast<int>(i);
}

// --- the settings screen ---------------------------------------------------------------------

struct Settings {
    Menu menu;
    rd::SettingsArt art;
    std::string bindings_path = rd::vk::default_bindings_path();
    bool bindings_dirty = false;

    void load_art(const fs::path& dir) {
        if (dir.empty())
            return;
        auto picture = [&](const char* name, rd::Image& out) {
            std::string err;
            if (!rd::vk::load_png(utf8(dir / name), out, &err))
                std::fprintf(stderr, "settings picture %s: %s\n", name, err.c_str());
        };
        picture("controller.png", art.controller);
        picture("controller_back.png", art.controller_back);
        picture("vmu.png", art.vmu);
        art.front = rd::parse_hotspots(read_file(dir / "controller.hotspots"));
        art.back = rd::parse_hotspots(read_file(dir / "controller_back.hotspots"));
        art.lcd = rd::parse_lcd_placement(read_file(dir / "vmu.hotspots"));
    }

    // A missing file is the ordinary first run: the defaults are what every game starts with.
    rd::Bindings load_bindings() const {
        const std::string text = read_file(from_utf8(bindings_path.c_str()));
        if (text.empty())
            return rd::Bindings::defaults();
        std::string warnings;
        rd::Bindings b = rd::Bindings::from_text(text, &warnings);
        if (!warnings.empty())
            std::fprintf(stderr, "%s:\n%s", bindings_path.c_str(), warnings.c_str());
        return b;
    }

    // The cards in the save directory, and what the selected game's slots hold.
    void refresh_cards(State& s, const Options& o) {
        std::vector<std::string> files;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(o.saves, ec))
            if (e.is_regular_file(ec) && e.file_size(ec) == dream::maple::MemoryCard::kImageSize)
                files.push_back(utf8(e.path().filename()));
        std::sort(files.begin(), files.end());
        menu.set_card_files(std::move(files));
        menu.set_rumble_available(false);
        const Game* g = s.game();
        for (unsigned slot = 0; slot < Menu::kSlots; ++slot) {
            rd::SlotView v;
            if (g) {
                v.file = s.slots(g->id)[slot];
                const fs::path p = o.saves / v.file;
                if (!v.file.empty() &&
                    fs::file_size(p, ec) == dream::maple::MemoryCard::kImageSize) {
                    const std::string bytes = read_file(p);
                    v.contents = dream::maple::read_vmu(
                        std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
                }
            }
            menu.set_slot(slot, std::move(v));
        }
    }

    // A slot changed on the screen: the game will be started with that card. A new card gets the
    // first free name and is made by the game the first time it starts with it.
    void swap(State& s, const Options& o) {
        const Game* g = s.game();
        if (!g)
            return;
        std::string& file = s.slots(g->id)[menu.swap_slot()];
        const Menu::Choice& c = menu.swap_choice();
        switch (c.kind) {
            case Menu::Choice::Kind::Empty:
            case Menu::Choice::Kind::Rumble:
                file.clear();
                break;
            case Menu::Choice::Kind::Card:
                file = c.file;
                break;
            case Menu::Choice::Kind::NewCard:
                for (unsigned n = 1; n < 1000; ++n) {
                    file = g->id + "-vmu-" + std::to_string(n) + ".bin";
                    std::error_code ec;
                    if (!fs::exists(o.saves / file, ec))
                        break;
                }
                break;
        }
        save_cards(s, o);
        refresh_cards(s, o);
    }

    void open(rd::vk::Window& w, State& s, const Options& o) {
        menu.open(w.bindings(), Menu::Tab::Controller);
        refresh_cards(s, o);
        bindings_dirty = false;
    }

    void close(rd::vk::Window& w) {
        menu.close();
        w.cancel_capture();
        if (!bindings_dirty)
            return;
        bindings_dirty = false;
        std::ofstream out(from_utf8(bindings_path.c_str()), std::ios::binary);
        const std::string text = menu.bindings().to_text();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out)
            std::fprintf(stderr, "cannot write the bindings to %s\n", bindings_path.c_str());
    }

    // Applied live, so a rebound control works at once; the file is written on closing.
    void apply(rd::vk::Window& w) {
        if (!menu.dirty())
            return;
        menu.mark_saved();
        bindings_dirty = true;
        w.set_bindings(menu.bindings());
    }

    // The in-game screen's handling, key for key (boot_main's pump_menu).
    void pump(rd::vk::Window& w, State& s, const Options& o) {
        rd::Binding captured;
        bool cancelled = false;
        if (w.take_capture(captured, cancelled)) {
            menu.apply_capture(captured, cancelled);
            apply(w);
            return;
        }
        if (w.pressed(Control::Menu)) {
            close(w);
            return;
        }
        if (w.pressed(Control::Back)) {
            menu.back();  // one leaves a capture or a pending choice, a second the screen
            if (!menu.is_open())
                close(w);
            return;
        }
        if (w.pressed(Control::PrevTab))
            menu.switch_tab(-1);
        if (w.pressed(Control::NextTab))
            menu.switch_tab(1);
        if (w.pressed(Control::Up))
            menu.move(-1);
        if (w.pressed(Control::Down))
            menu.move(1);
        const unsigned devices = static_cast<unsigned>(w.devices().size());
        if (w.pressed(Control::Left))
            menu.adjust(-1, devices);
        if (w.pressed(Control::Right))
            menu.adjust(1, devices);
        if (w.pressed(Control::Start) || w.pressed(Control::A)) {
            const Menu::Action action = menu.activate(devices);
            if (action == Menu::Action::Close) {
                close(w);
                return;
            }
            if (action == Menu::Action::Swap)
                swap(s, o);
            if (menu.awaiting())
                w.begin_capture();
        }
        apply(w);
    }

    rd::PadPreview preview(const rd::vk::Window& w) const {
        rd::PadPreview p;
        for (unsigned i = 0; i < rd::kPadControlCount; ++i) {
            const auto c = static_cast<rd::PadControl>(i);
            p.held[i] = w.pad_held(c);
            p.value[i] = w.pad_value(c);
        }
        return p;
    }
};

// --- starting a game -------------------------------------------------------------------------

// Starts the game with its cards and waits for it with the launcher hidden. The game's output
// goes to <logs>/<id>.log, the way the batch files did it.
void run_game(rd::vk::Window& w, const Options& o, const Game& g, State& s) {
    std::error_code ec;
    fs::create_directories(o.saves, ec);
    fs::create_directories(o.logs, ec);
    const Slots cards = s.slots(g.id);
    std::vector<std::string> args = {utf8(g.exe), "--config",  utf8(g.config),
                                     "--window",  "--vmu-dir", utf8(o.saves)};
    if (!cards[0].empty()) {
        args.push_back("--vmu");
        args.push_back(utf8(o.saves / cards[0]));
    }
    if (!cards[1].empty()) {
        args.push_back("--vmu2");
        args.push_back(utf8(o.saves / cards[1]));
    }
    if (!o.ui_assets.empty()) {
        args.push_back("--ui-assets");
        args.push_back(utf8(o.ui_assets));
    }
    // What the session reaches that the translation lacks, for after_play.
    const fs::path suggest = o.logs / (g.id + "-play-suggest.toml");
    fs::remove(suggest, ec);
    args.push_back("--suggest-config");
    args.push_back(utf8(suggest));
    std::vector<const char*> argv;
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    const std::string log_path = utf8(o.logs / (g.id + ".log"));
    SDL_IOStream* log = SDL_IOFromFile(log_path.c_str(), "w");
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    const std::string cwd = utf8(o.root);
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, cwd.c_str());
    if (log) {
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                              SDL_PROCESS_STDIO_REDIRECT);
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, log);
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    }
    SDL_Process* proc = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!proc) {
        s.message = "IMPOSSIBLE DE LANCER " + upper(g.id) + ": " + upper(SDL_GetError());
        if (log)
            SDL_CloseIO(log);
        return;
    }
    SDL_HideWindow(w.sdl_window());
    int code = 0;
    while (!SDL_WaitProcess(proc, false, &code)) {
        SDL_PumpEvents();
        SDL_Delay(100);
    }
    SDL_DestroyProcess(proc);
    if (log)
        SDL_CloseIO(log);
    SDL_ShowWindow(w.sdl_window());
    SDL_RaiseWindow(w.sdl_window());
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    w.poll();  // forget what was held when the game started
    s.message = code == 0 ? std::string{}
                          : "LE JEU A QUITTE AVEC LE CODE " + std::to_string(code) +
                                ". JOURNAL: " + upper(g.id) + ".LOG";
}

// --- adding a game ---------------------------------------------------------------------------

using dream::launcher::ImportStatus;

// The file dialog's answer. It may come from the dialog's own thread.
struct Pick {
    std::mutex mutex;
    bool open = false, answered = false;
    std::string file;  // UTF-8; empty when the dialog was closed without a choice
};

void SDLCALL on_picked(void* userdata, const char* const* files, int) {
    Pick& p = *static_cast<Pick*>(userdata);
    std::lock_guard<std::mutex> lock(p.mutex);
    p.answered = true;
    p.file = files && files[0] ? files[0] : "";
}

// A game being added, or built again after a session found something: the work runs on a thread
// of its own, shown over the library.
struct Import {
    Pick pick;
    std::string start_dir;                 // the dialog reads it until it answers
    std::unique_ptr<ImportStatus> status;  // set while something runs
    std::thread worker;
    bool update = false;  // building a game again, rather than adding one
    std::string subject;  // the disc's file name, or the game's title
    std::uint64_t started = 0;
    bool busy() const { return status != nullptr; }
};

// A value from the build directory's CMakeCache.txt, "" when it is not there.
std::string cache_value(const fs::path& build, std::string_view key) {
    std::ifstream in(build / "CMakeCache.txt");
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t colon = line.find(':'), eq = line.find('=');
        if (colon != std::string::npos && eq != std::string::npos &&
            line.compare(0, colon, key) == 0)
            return line.substr(eq + 1);
    }
    return {};
}

// What adding a game needs that is missing here, as the message line says it; "" when nothing is.
std::string import_blocker(const Options& o) {
    std::error_code ec;
    if (!fs::exists(o.build / "CMakeCache.txt", ec))
        return "PAS DE DOSSIER DE COMPILATION ICI: " + upper(utf8(o.build));
#ifdef _WIN32
    // MSVC finds its headers and libraries through what vcvars64.bat puts in the environment.
    const std::string compiler = cache_value(o.build, "CMAKE_CXX_COMPILER");
    if (compiler.size() >= 6 && compiler.compare(compiler.size() - 6, 6, "cl.exe") == 0 &&
        !SDL_getenv("VCINSTALLDIR"))
        return "POUR AJOUTER UN JEU, OUVRIR LE LANCEUR PAR LANCEUR-DREAM-RECOMP.BAT";
#endif
    return {};
}

// Stops a program and what it started: CMake's Ninja and the compilers under it.
void stop_process(SDL_Process* proc) {
#ifdef _WIN32
    const Sint64 pid =
        SDL_GetNumberProperty(SDL_GetProcessProperties(proc), SDL_PROP_PROCESS_PID_NUMBER, 0);
    if (pid > 0) {
        const std::string id = std::to_string(pid);
        const char* args[] = {"taskkill", "/F", "/T", "/PID", id.c_str(), nullptr};
        const SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
        if (SDL_Process* kill = SDL_CreateProcessWithProperties(props)) {
            SDL_WaitProcess(kill, true, nullptr);
            SDL_DestroyProcess(kill);
        }
        SDL_DestroyProperties(props);
    }
#endif
    SDL_KillProcess(proc, true);
}

// The importer's Runner: the program's output goes to `log`; it is stopped when it runs past
// `timeout_s` or the import is cancelled.
int run_logged(const std::vector<std::string>& args, const fs::path& cwd, const fs::path& log_path,
               unsigned timeout_s, const std::atomic<bool>& cancel) {
    std::vector<const char*> argv;
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    SDL_IOStream* log = SDL_IOFromFile(utf8(log_path).c_str(), "w");
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    const std::string dir = utf8(cwd);
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, dir.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    if (log) {
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                              SDL_PROCESS_STDIO_REDIRECT);
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, log);
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    }
    SDL_Process* proc = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    std::string note;
    int code = -1;
    if (!proc) {
        note = "[lanceur] impossible de lancer " + args[0] + ": " + SDL_GetError() + "\n";
    } else {
        const std::uint64_t deadline = SDL_GetTicks() + std::uint64_t{timeout_s} * 1000;
        while (!SDL_WaitProcess(proc, false, &code)) {
            if (cancel || SDL_GetTicks() > deadline) {
                note = cancel ? "\n[lanceur] annule\n"
                              : "\n[lanceur] arrete apres " + std::to_string(timeout_s) + " s\n";
                stop_process(proc);
                SDL_WaitProcess(proc, true, &code);
                code = -1;
                break;
            }
            SDL_Delay(200);
        }
        SDL_DestroyProcess(proc);
    }
    if (log)
        SDL_CloseIO(log);
    if (!note.empty())
        std::ofstream(log_path, std::ios::binary | std::ios::app) << note;
    return code;
}

void ask_for_disc(rd::vk::Window& w, Import& imp, const Options& o, State& s) {
    if (const std::string why = import_blocker(o); !why.empty()) {
        s.message = why;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(imp.pick.mutex);
        if (imp.pick.open)
            return;
        imp.pick.open = true;
    }
    static const SDL_DialogFileFilter kFilters[] = {
        {"Disque Dreamcast (GDI, CDI, CHD)", "gdi;cdi;chd"}};
    imp.start_dir = utf8(o.discs);
    SDL_ShowOpenFileDialog(on_picked, &imp.pick, w.sdl_window(), kFilters, 1,
                           imp.start_dir.empty() ? nullptr : imp.start_dir.c_str(), false);
}

dream::launcher::ImportOptions job_options(const Options& o) {
    dream::launcher::ImportOptions io;
    io.root = o.root;
    io.build = o.build;
    io.dcdisc = o.dcdisc;
    io.logs = o.logs;
    // The CMake that made the build directory, so nothing has to be on PATH for it.
    if (std::string cmake = cache_value(o.build, "CMAKE_COMMAND"); !cmake.empty())
        io.cmake = std::move(cmake);
    return io;
}

// Runs `work` on the worker thread with programs started by run_logged.
template <typename Work>
void start_job(Import& imp, bool update, std::string subject, Work work) {
    imp.status = std::make_unique<ImportStatus>();
    imp.update = update;
    imp.subject = std::move(subject);
    imp.started = SDL_GetTicks();
    ImportStatus* status = imp.status.get();
    imp.worker = std::thread([work, status] {
        work(
            [status](const std::vector<std::string>& args, const fs::path& cwd, const fs::path& log,
                     unsigned timeout_s) {
                return run_logged(args, cwd, log, timeout_s, status->cancel);
            },
            *status);
    });
}

void start_import(Import& imp, const Options& o, const std::string& disc) {
    dream::launcher::ImportOptions io = job_options(o);
    io.disc = from_utf8(disc.c_str());
    start_job(imp, false, utf8(io.disc.filename()),
              [io](const dream::launcher::Runner& run, ImportStatus& status) {
                  dream::launcher::import_game(io, run, status);
              });
}

// After a session: what the game reached that its translation lacks goes into its configuration,
// and the game is built again, so the next session runs that code translated instead of through
// the interpreter. A configuration newer than the executable (a build stopped half way, or
// launched without the compilers) is built too.
void after_play(Import& imp, State& s, const Options& o, const Game& g) {
    const dream::launcher::Findings f =
        dream::launcher::fold_findings(g.config, o.logs / (g.id + "-play-suggest.toml"));
    if (!f.error.empty()) {
        std::fprintf(stderr, "%s: %s\n", g.id.c_str(), f.error.c_str());
        return;
    }
    std::error_code e1, e2;
    const auto config_time = fs::last_write_time(g.config, e1);
    const auto exe_time = fs::last_write_time(g.exe, e2);
    const bool stale = !e1 && !e2 && config_time > exe_time;
    const unsigned found = f.functions + f.relocations;
    if (found == 0 && !stale)
        return;
    std::string done = upper(g.id) + " AMELIORE";
    if (found)
        done += ": " + std::to_string(found) + (found == 1 ? " AJOUT TROUVE" : " AJOUTS TROUVES") +
                " EN JOUANT";
    if (!import_blocker(o).empty()) {
        s.message = "TROUVE EN JOUANT, NOTE POUR " + upper(g.id) +
                    ": COMPILE QUAND LE LANCEUR EST OUVERT PAR LE .BAT";
        return;
    }
    dream::launcher::ImportOptions io = job_options(o);
    const std::string id = g.id;
    start_job(imp, true, g.title,
              [io, id, done](const dream::launcher::Runner& run, ImportStatus& status) {
                  dream::launcher::update_game(io, id, done, run, status);
              });
}

// The dialog answered, or the import ended: what the library shows changes.
void poll_import(Import& imp, State& s, const Options& o) {
    std::string picked;
    bool answered = false;
    {
        std::lock_guard<std::mutex> lock(imp.pick.mutex);
        if (imp.pick.answered) {
            answered = true;
            imp.pick.answered = imp.pick.open = false;
            picked = std::move(imp.pick.file);
        }
    }
    if (answered && !picked.empty() && !imp.busy()) {
        s.message.clear();
        start_import(imp, o, picked);
    }
    if (!imp.busy())
        return;
    std::string id, message;
    fs::path log;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(imp.status->mutex);
        if (!imp.status->done)
            return;
        id = imp.status->id;
        message = imp.status->message;
        log = imp.status->log;
        ok = imp.status->ok;
    }
    imp.worker.join();
    imp.status.reset();
    rescan(s, o);
    for (std::size_t i = 0; i < s.games.size(); ++i)
        if (s.games[i].id == id)
            s.sel = static_cast<int>(i);
    s.action = ok || imp.update ? kPlay : kAdd;  // a game not rebuilt still plays as it was
    s.message = message;
    if (!ok && !log.empty())
        s.message += ". JOURNAL: " + upper(utf8(log.filename()));
}

void stop_import(Import& imp) {
    if (!imp.busy())
        return;
    imp.status->cancel = true;
    imp.worker.join();
    imp.status.reset();
}

// The screen over the library while a game is added or built again.
void draw_import(Canvas& cv, bool update, std::string_view subject, std::string_view step,
                 std::string_view id, std::uint64_t elapsed_ms, bool cancelling) {
    constexpr int bw = 720, bh = 260, bx = (kW - bw) / 2, by = (kH - bh) / 2;
    cv.fill(bx - 3, by - 3, bw + 6, bh + 6, kFocus);
    cv.fill(bx, by, bw, bh, kPanel);
    const int x = bx + 24, w = bw - 48;
    cv.text(x, by + 20, update ? "AMELIORATION DU JEU" : "AJOUT D UN JEU", 3, kWhite);
    cv.text(x, by + 60, rd::fit_text(upper(std::string(subject)), w, 2), 2, kGrey);
    if (!id.empty())
        cv.text(x, by + 84, rd::fit_text("NOM: " + upper(std::string(id)), w, 2), 2, kGrey);
    cv.text(x, by + 120, rd::fit_text(cancelling ? "ARRET EN COURS..." : step, w, 2), 2,
            cancelling ? kWarn : kLight);
    // No real progress to show: a block moves along the bar while something runs.
    cv.fill(x, by + 152, w, 12, kButton);
    const int block = 120, travel = w - block;
    const int pos = static_cast<int>(elapsed_ms / 8 % static_cast<std::uint64_t>(2 * travel));
    cv.fill(x + (pos < travel ? pos : 2 * travel - pos), by + 152, block, 12, kFocus);
    const unsigned secs = static_cast<unsigned>(elapsed_ms / 1000);
    char clock[32];
    std::snprintf(clock, sizeof clock, "TEMPS: %u:%02u", secs / 60, secs % 60);
    cv.text(x, by + 180, clock, 2, kGrey);
    cv.text(x, by + 204,
            update ? "RECOMPILE AVEC CE QUE LA PARTIE A TROUVE"
                   : "LA PREMIERE COMPILATION PEUT PRENDRE PLUSIEURS MINUTES",
            2, kGrey);
    cv.text(x, by + 228, update ? "B: PLUS TARD" : "B: ANNULER", 2, kGrey);
}

void draw_import(Canvas& cv, const Import& imp) {
    std::string step, id;
    {
        std::lock_guard<std::mutex> lock(imp.status->mutex);
        step = imp.status->step;
        id = imp.status->id;
    }
    draw_import(cv, imp.update, imp.subject, step, id, SDL_GetTicks() - imp.started,
                imp.status->cancel);
}

// --import: the same without a window, each step printed as it starts.
int import_headless(const Options& o) {
    if (const std::string why = import_blocker(o); !why.empty()) {
        std::fprintf(stderr, "%s\n", why.c_str());
        return 1;
    }
    Import imp;
    start_import(imp, o, utf8(o.import_disc));
    std::string last;
    for (bool done = false; !done;) {
        std::string step;
        {
            std::lock_guard<std::mutex> lock(imp.status->mutex);
            step = imp.status->step;
            done = imp.status->done;
        }
        if (!step.empty() && step != last) {
            const unsigned secs = static_cast<unsigned>((SDL_GetTicks() - imp.started) / 1000);
            std::printf("[%u:%02u] %s\n", secs / 60, secs % 60, step.c_str());
            std::fflush(stdout);
            last = step;
        }
        if (!done)
            SDL_Delay(250);
    }
    imp.worker.join();
    std::printf("%s\n", imp.status->message.c_str());
    if (!imp.status->log.empty())
        std::printf("journal: %s\n", utf8(imp.status->log).c_str());
    return imp.status->ok ? 0 : 1;
}

// --- the library's own keys ------------------------------------------------------------------

void move(State& s, int dsel, int daction) {
    const int n = static_cast<int>(s.games.size());
    if (n > 0)
        s.sel = std::clamp(s.sel + dsel, 0, n - 1);
    s.action = std::clamp(s.action + daction, 0, kActions - 1);
    s.message.clear();
}

void pump_library(rd::vk::Window& w, State& s, Settings& st, Import& imp, const Options& o,
                  bool& running) {
    if (w.pressed(Control::Up))
        move(s, -1, 0);
    if (w.pressed(Control::Down))
        move(s, 1, 0);
    if (w.pressed(Control::Left))
        move(s, 0, -1);
    if (w.pressed(Control::Right))
        move(s, 0, 1);
    if (w.pressed(Control::Menu)) {
        st.open(w, s, o);
        return;
    }
    if (w.pressed(Control::Back)) {
        // B or escape goes to QUITTER; a second one leaves.
        if (s.action == kQuit)
            running = false;
        s.action = kQuit;
        return;
    }
    if (!w.pressed(Control::A) && !w.pressed(Control::Start))
        return;
    if (s.action == kQuit) {
        running = false;
        return;
    }
    if (s.action == kSettings) {
        st.open(w, s, o);
        return;
    }
    if (s.action == kAdd) {
        ask_for_disc(w, imp, o, s);
        return;
    }
    const Game* g = s.game();
    if (!g)
        return;
    if (!g->built)
        s.message = "PAS ENCORE COMPILE: CONSTRUIRE " + upper(g->id) + "_BOOT";
    else if (!g->disc)
        s.message = "DISQUE ABSENT: VOIR GAMES/" + upper(g->id) + "/DISC";
    else {
        run_game(w, o, *g, s);
        after_play(imp, s, o, *g);
    }
}

void present(rd::vk::Window& w, rd::vk::Presenter& p, const Canvas& cv) {
    std::uint32_t image = 0;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!w.begin_frame(image, cmd)) {
        w.recreate_swapchain();
        return;
    }
    p.upload(cmd, cv.px.data(), kW, kH);
    VkClearValue clears[2]{};
    clears[0].color = {{0.06f, 0.08f, 0.12f, 1.0f}};
    clears[1].depthStencil = {0.0f, 0};
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = w.render_pass();
    rpbi.framebuffer = w.framebuffer(image);
    rpbi.renderArea.extent = w.extent();
    rpbi.clearValueCount = 2;
    rpbi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{
        0.0f, 0.0f, static_cast<float>(w.extent().width), static_cast<float>(w.extent().height),
        0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, w.extent()};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    p.draw(cmd, w.extent());
    vkCmdEndRenderPass(cmd);
    if (!w.end_frame(image))
        w.recreate_swapchain();
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const bool more = i + 1 < argc;
        if (!std::strcmp(argv[i], "--root") && more)
            o.root = from_utf8(argv[++i]);
        else if (!std::strcmp(argv[i], "--build") && more)
            o.build = from_utf8(argv[++i]);
        else if (!std::strcmp(argv[i], "--saves") && more)
            o.saves = from_utf8(argv[++i]);
        else if (!std::strcmp(argv[i], "--logs") && more)
            o.logs = from_utf8(argv[++i]);
        else if (!std::strcmp(argv[i], "--ui-assets") && more)
            o.ui_assets = from_utf8(argv[++i]);
        else if (!std::strcmp(argv[i], "--dcdisc") && more)
            o.dcdisc = from_utf8(argv[++i]);
        else if (!std::strcmp(argv[i], "--discs") && more)
            o.discs = from_utf8(argv[++i]);
        else if (!std::strcmp(argv[i], "--import") && more)
            o.import_disc = from_utf8(argv[++i]);
        else if (!std::strcmp(argv[i], "--screenshot") && more)
            o.screenshot = argv[++i];
        else if (!std::strcmp(argv[i], "--select") && more)
            o.select = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--tab") && more)
            o.tab = argv[++i];
        else {
            std::printf(
                "usage: dream_launcher [--root DIR] [--build DIR] [--saves DIR] [--logs DIR]\n"
                "                      [--ui-assets DIR] [--dcdisc PROGRAM] [--discs DIR]\n"
                "       dream_launcher --import DISC   add a game without a window\n"
                "       dream_launcher --screenshot FILE.ppm [--select N] [--tab T]\n"
                "  --root       the checkout, where games/ is (default: the one it was built "
                "from)\n"
                "  --build      the build directory (default: the one this executable is in)\n"
                "  --saves      memory cards; a game's own is <id>-vmu.bin (default: user data)\n"
                "  --logs       each game's output, <id>.log (default: user data)\n"
                "  --ui-assets  pictures for the settings screen\n"
                "  --dcdisc     the disc tool, for adding a game (default: the checkout's .venv)\n"
                "  --discs      where the dialog for adding a game opens\n"
                "  --screenshot draw one frame to a PPM file and exit; --tab controller or memory\n"
                "               draws the settings screen over it, --tab import or update the\n"
                "               screen shown while a game is added or built again\n");
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse_args(argc, argv, o))
        return 2;
    if (o.root.empty())
        o.root = from_utf8(DREAM_LAUNCHER_ROOT);
    if (o.build.empty()) {
        const char* base = SDL_GetBasePath();  // the launcher's directory, build/launcher/
        o.build = base ? from_utf8(base).parent_path().parent_path() : fs::current_path();
    }
    if (o.saves.empty() || o.logs.empty()) {
        char* pref = SDL_GetPrefPath("dream-recomp", "dream-recomp");
        const fs::path data = pref ? from_utf8(pref) : fs::current_path();
        SDL_free(pref);
        if (o.saves.empty())
            o.saves = data / "cards";
        if (o.logs.empty())
            o.logs = data / "logs";
    }
    if (o.dcdisc.empty()) {
        std::error_code ec;
        for (const fs::path& p : {o.root / ".venv/Scripts/dcdisc.exe", o.root / ".venv/bin/dcdisc"})
            if (fs::exists(p, ec)) {
                o.dcdisc = p;
                break;
            }
        if (o.dcdisc.empty())
            o.dcdisc = "dcdisc";  // from PATH
    }
    if (!o.import_disc.empty())
        return import_headless(o);

    State s;
    load_cards(s, o);
    rescan(s, o);
    Settings st;
    st.load_art(o.ui_assets);
    Canvas cv;
    if (!o.screenshot.empty()) {
        s.sel = std::clamp(o.select, 0, std::max(0, static_cast<int>(s.games.size()) - 1));
        draw(cv, s);
        if (o.tab == "import" || o.tab == "update") {
            const bool update = o.tab == "update";
            draw_import(cv, update,
                        update ? "Tokyo Highway Challenge" : "Demo Disc v1.000 (2000)(PAL)[!].gdi",
                        update ? "TRADUCTION ET COMPILATION" : "TRADUCTION ET COMPILATION (1/3)",
                        update ? "tokyohighwaychallenge" : "demo", 754000, false);
        } else if (!o.tab.empty()) {
            st.menu.open(st.load_bindings(),
                         o.tab == "memory" ? Menu::Tab::Memory : Menu::Tab::Controller);
            st.refresh_cards(s, o);
            st.menu.draw(cv.px.data(), kW, kW, kH, {"KEYBOARD"}, rd::PadPreview{}, st.art);
        }
        return write_ppm(o.screenshot, cv) ? 0 : 1;
    }

    rd::vk::Window w;
    if (!w.create("dream-recomp", kW, kH, false)) {
        std::fprintf(stderr, "window: %s\n", w.error().c_str());
        return 1;
    }
    rd::vk::Presenter p;
    if (!p.create(w.context(), w.render_pass())) {
        std::fprintf(stderr, "presenter: %s\n", p.error().c_str());
        return 1;
    }
    w.set_bindings(st.load_bindings());
    w.set_escape_quits(false);  // escape goes back a step, as in the game's settings screen

    Import imp;
    bool running = true;
    while (running && w.poll()) {
        poll_import(imp, s, o);
        if (imp.busy()) {
            if (w.pressed(Control::Back))
                imp.status->cancel = true;
        } else if (st.menu.is_open()) {
            st.pump(w, s, o);
        } else {
            pump_library(w, s, st, imp, o, running);
        }
        draw(cv, s);
        if (st.menu.is_open())
            st.menu.draw(cv.px.data(), kW, kW, kH, w.devices(), st.preview(w), st.art);
        if (imp.busy())
            draw_import(cv, imp);
        present(w, p, cv);
    }
    stop_import(imp);  // closing the window stops an import half done
    vkDeviceWaitIdle(w.context().device());
    p.destroy();
    w.destroy();
    return 0;
}
