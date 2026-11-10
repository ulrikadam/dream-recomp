// dream_launcher: the game library window (WP3.6).
//
// Lists the games under games/, with how far each one gets, whether it is built and whether its
// disc is there, and starts one with the memory cards plugged into the pad. Five screens share a
// bar along the bottom: JOUER (the library), AJOUTER UN JEU, CARTES VMU, MANETTE and QUITTER. The
// shoulder buttons, Q and W, or a click go from one to the next. MANETTE is the games' binding
// editor (rd::InputMenu) drawn the launcher's way; the bindings go to the file every game reads.
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
// Memory cards are shared, as real ones are (cards.h): VMU 1, 2... in the save directory, any game
// on any card. The pad's two slots, A1 and A2, hold the same cards for every game, VMU 1 in A1 by
// default. CARTES VMU shows them with a card drawn by the program (render/vmu_art.h), its saves
// on the card's screen and listed beside it; it copies or deletes saves, makes new cards and plugs
// one into a slot. The first start gathers the old one-card-per-game saves onto VMU 1.
//
//   dream_launcher [--root DIR] [--build DIR] [--saves DIR] [--logs DIR] [--ui-assets DIR]
//                  [--dcdisc PROGRAM] [--discs DIR]
//   dream_launcher --import DISC     adds a game without a window, printing each step
//   dream_launcher --screenshot FILE.ppm [--select N] [--tab T]
//       draws one frame to a file, no window. T: play, add, cards (or cards-menu, cards-delete,
//       cards-new), pad, quit; import or update draw adding a game or building one again
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
#include "dream/render/vmu_art.h"
#include "dream/runtime/maple/maple.h"
#include "dream/runtime/maple/vmu_fs.h"

#include "cards.h"
#include "discs.h"
#include "importer.h"
#include "library.h"
#include <SDL3/SDL.h>

namespace {

namespace fs = std::filesystem;
namespace rd = dream::render;
using dream::launcher::Game;
using dream::launcher::Level;
using Control = rd::vk::Control;

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

// The screens, in the order of the bar along the bottom of every one of them. The shoulder buttons
// (Q and W on the keyboard) or a click on the bar go from one to the next, and the window shows it
// at once.
enum Action : int { kPlay, kAdd, kCards, kPad, kQuit, kActions };
constexpr std::string_view kActionLabels[kActions] = {"JOUER", "AJOUTER UN JEU", "CARTES VMU",
                                                      "MANETTE", "QUITTER"};
constexpr std::string_view kHelp = "HAUT/BAS: JEU  A: JOUER  LB/RB OU Q/W: MENU  B: QUITTER";

// Where things go: each screen above the bar, its help or message on one line at the bottom of
// it, and the bar below.
constexpr int kContentH = 480, kHelpY = 450, kBarY = 494, kBarH = 32;

// The cards plugged into the pad's two slots, A1 and A2: a file name each, "" for an empty slot.
// The same for every game, as on the console: a player plugs a VMU in and every game finds it.
using Slots = std::array<std::string, 2>;

struct State {
    std::vector<Game> games;
    int sel = 0, action = kPlay;
    std::string message;
    Slots ports{dream::launcher::kFirstCard, ""};

    const Game* game() const {
        return games.empty() ? nullptr : &games[static_cast<std::size_t>(sel)];
    }
};

// Which cards are plugged in lives beside the cards: "A1<TAB>file" and "A2<TAB>file".
fs::path ports_file(const Options& o) {
    return o.saves / "launcher-ports.txt";
}

void load_ports(State& s, const Options& o) {
    std::ifstream in(ports_file(o));
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos)
            continue;
        const std::string port = line.substr(0, tab), file = line.substr(tab + 1);
        if (port == "A1")
            s.ports[0] = file;
        else if (port == "A2")
            s.ports[1] = file;
    }
}

void save_ports(const State& s, const Options& o) {
    std::error_code ec;
    fs::create_directories(o.saves, ec);
    std::ofstream out(ports_file(o));
    out << "A1\t" << s.ports[0] << "\nA2\t" << s.ports[1] << '\n';
}

// "VMU 1", "VMU 1 + VMU 2", or "AUCUNE": what the pad has plugged in.
std::string ports_label(const Slots& p) {
    std::string out;
    for (const std::string& f : p)
        if (!f.empty())
            out += (out.empty() ? "" : " + ") + rd::card_label(f);
    return out.empty() ? "AUCUNE" : out;
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
        line("VMU", ports_label(s.ports), kLight);
    }

    if (s.message.empty())
        cv.text(24, kHelpY, kHelp, 2, kGrey);
    else
        cv.text(24, kHelpY, rd::fit_text(s.message, kW - 48, 2), 2, kWarn);
}

// --- the bar along the bottom ----------------------------------------------------------------

struct BarSlot {
    int x = 0, w = 0;
};

// The buttons, centred.
std::array<BarSlot, kActions> bar_slots() {
    std::array<BarSlot, kActions> slots{};
    int total = 0;
    for (int a = 0; a < kActions; ++a) {
        slots[a].w = width_of(kActionLabels[a], 2) + 24;
        total += slots[a].w + (a ? 12 : 0);
    }
    int x = (kW - total) / 2;
    for (BarSlot& b : slots) {
        b.x = x;
        x += b.w + 12;
    }
    return slots;
}

void draw_bar(Canvas& cv, int current) {
    cv.fill(0, kContentH, kW, kH - kContentH, kHeader);
    const std::array<BarSlot, kActions> slots = bar_slots();
    for (int a = 0; a < kActions; ++a) {
        const bool here = a == current;
        cv.fill(slots[a].x, kBarY, slots[a].w, kBarH, here ? kFocus : kButton);
        cv.text(slots[a].x + 12, kBarY + 9, kActionLabels[a], 2, here ? kInk : kWhite);
    }
    // The shoulder buttons that step along it.
    cv.text(slots.front().x - 16 - width_of("LB", 2), kBarY + 9, "LB", 2, kGrey);
    cv.text(slots.back().x + slots.back().w + 16, kBarY + 9, "RB", 2, kGrey);
}

// The screen whose button is at (x, y), or -1.
int bar_hit(int x, int y) {
    if (y < kBarY || y >= kBarY + kBarH)
        return -1;
    const std::array<BarSlot, kActions> slots = bar_slots();
    for (int a = 0; a < kActions; ++a)
        if (x >= slots[a].x && x < slots[a].x + slots[a].w)
            return a;
    return -1;
}

void draw_quit(Canvas& cv) {
    cv.fill(0, 0, kW, kContentH, kBg);
    cv.fill(0, 0, kW, 56, kHeader);
    cv.text(24, 12, "QUITTER", 4, kWhite);
    constexpr int bw = 600, bh = 170, bx = (kW - bw) / 2, by = 150;
    cv.fill(bx, by, bw, bh, kPanel);
    const std::string_view q = "FERMER DREAM-RECOMP ?";
    cv.text(bx + (bw - width_of(q, 3)) / 2, by + 30, q, 3, kWhite);
    const std::string_view saved = "TES SAUVEGARDES SONT DEJA SUR TES VMU.";
    cv.text(bx + (bw - width_of(saved, 2)) / 2, by + 78, saved, 2, kGrey);
    const std::string_view keys = "A: OUI, FERMER    B: NON";
    cv.text(bx + (bw - width_of(keys, 2)) / 2, by + 122, keys, 2, kFocus);
    cv.text(24, kHelpY, "A: FERMER  LB/RB OU Q/W: MENU  B: RETOUR AUX JEUX", 2, kGrey);
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

// --- the pad screen --------------------------------------------------------------------------
//
// The games' binding editor (rd::InputMenu) drawn the launcher's way and in its language: a
// picture of the pad on the left, the control being changed ringed and those held lit, and the
// rows on the right. The bindings are the file every game reads; a change works at once and is
// written on leaving the screen.

struct PadScreen {
    rd::InputMenu input;
    rd::Image front, back;  // the pictures, scaled once to their places
    rd::Hotspots front_spots{}, back_spots{};
    std::string bindings_path = rd::vk::default_bindings_path();
    bool bindings_dirty = false;

    void load_art(const fs::path& dir) {
        if (dir.empty())
            return;
        auto picture = [&](const char* name, unsigned w, unsigned h) {
            rd::Image img;
            std::string err;
            if (!rd::vk::load_png(utf8(dir / name), img, &err)) {
                std::fprintf(stderr, "pad picture %s: %s\n", name, err.c_str());
                return rd::Image{};
            }
            return rd::scale_to_fit(img, w, h);
        };
        front = picture("controller.png", 340, 236);
        back = picture("controller_back.png", 170, 70);
        front_spots = rd::parse_hotspots(read_file(dir / "controller.hotspots"));
        back_spots = rd::parse_hotspots(read_file(dir / "controller_back.hotspots"));
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

    bool is_open() const { return input.is_open(); }

    void open(rd::vk::Window& w) {
        input.open(w.bindings());
        bindings_dirty = false;
    }

    void close(rd::vk::Window& w) {
        input.close();
        w.cancel_capture();
        if (!bindings_dirty)
            return;
        bindings_dirty = false;
        std::ofstream out(from_utf8(bindings_path.c_str()), std::ios::binary);
        const std::string text = input.bindings().to_text();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out)
            std::fprintf(stderr, "cannot write the bindings to %s\n", bindings_path.c_str());
    }

    // Applied live, so a rebound control works at once; the file is written on closing.
    void apply(rd::vk::Window& w) {
        if (!input.dirty())
            return;
        input.mark_saved();
        bindings_dirty = true;
        w.set_bindings(input.bindings());
    }

    // As the games' own screen handles it: B leaves a capture, then the screen.
    void pump(rd::vk::Window& w) {
        rd::Binding captured;
        bool cancelled = false;
        if (w.take_capture(captured, cancelled)) {
            input.apply_capture(captured, cancelled);
            apply(w);
            return;
        }
        if (w.pressed(Control::Menu)) {
            close(w);
            return;
        }
        if (w.pressed(Control::Back)) {
            input.back();
            if (!input.is_open())
                close(w);
            return;
        }
        if (w.pressed(Control::Up))
            input.move(-1);
        if (w.pressed(Control::Down))
            input.move(1);
        const unsigned devices = static_cast<unsigned>(w.devices().size());
        if (w.pressed(Control::Left))
            input.adjust(-1, devices);
        if (w.pressed(Control::Right))
            input.adjust(1, devices);
        if (w.pressed(Control::Start) || w.pressed(Control::A)) {
            if (input.activate(devices) == rd::InputMenu::Action::Close) {
                close(w);
                return;
            }
            if (input.awaiting())
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

// The screen's words. The rows come from rd::InputMenu in the games' English; these are the
// launcher's.
const char* pad_name(rd::PadControl c) {
    using P = rd::PadControl;
    switch (c) {
        case P::Start:
            return "START";
        case P::Up:
            return "CROIX HAUT";
        case P::Down:
            return "CROIX BAS";
        case P::Left:
            return "CROIX GAUCHE";
        case P::Right:
            return "CROIX DROITE";
        case P::LeftTrigger:
            return "GACHETTE L";
        case P::RightTrigger:
            return "GACHETTE R";
        case P::StickUp:
            return "STICK HAUT";
        case P::StickDown:
            return "STICK BAS";
        case P::StickLeft:
            return "STICK GAUCHE";
        case P::StickRight:
            return "STICK DROITE";
        default:
            return rd::pad_control_label(c);  // A, B, X, Y
    }
}

// A key's or a pad input's name as the bindings file has it, said in the launcher's language
// where it is a word rather than a letter.
std::string input_name(const std::string& v) {
    static const std::map<std::string, std::string> kNames = {
        {"--", "AUCUNE"},
        {"RETURN", "ENTREE"},
        {"UP", "HAUT"},
        {"DOWN", "BAS"},
        {"LEFT", "GAUCHE"},
        {"RIGHT", "DROITE"},
        {"SPACE", "ESPACE"},
        {"ESCAPE", "ECHAP"},
        {"BACKSPACE", "EFFACER"},
        {"LEFT SHIFT", "MAJ GAUCHE"},
        {"RIGHT SHIFT", "MAJ DROITE"},
        {"LEFT CTRL", "CTRL GAUCHE"},
        {"RIGHT CTRL", "CTRL DROIT"},
        {"LEFT ALT", "ALT"},
        {"RIGHT ALT", "ALT GR"},
        {"DPUP", "CROIX HAUT"},
        {"DPDOWN", "CROIX BAS"},
        {"DPLEFT", "CROIX GAUCHE"},
        {"DPRIGHT", "CROIX DROITE"},
        {"LEFTSHOULDER", "LB"},
        {"RIGHTSHOULDER", "RB"},
        {"LEFTTRIGGER +", "LT"},
        {"RIGHTTRIGGER +", "RT"},
        {"LEFTX -", "STICK G GAUCHE"},
        {"LEFTX +", "STICK G DROITE"},
        {"LEFTY -", "STICK G HAUT"},
        {"LEFTY +", "STICK G BAS"},
        {"RIGHTX -", "STICK D GAUCHE"},
        {"RIGHTX +", "STICK D DROITE"},
        {"RIGHTY -", "STICK D HAUT"},
        {"RIGHTY +", "STICK D BAS"},
        {"BACK", "SELECT"},
        {"LEFTSTICK", "CLIC STICK G"},
        {"RIGHTSTICK", "CLIC STICK D"},
    };
    const auto it = kNames.find(v);
    return it == kNames.end() ? v : it->second;
}

std::string device_name(const std::string& d) {
    return d == "KEYBOARD" ? std::string("CLAVIER") : upper(d);
}

void draw_pad(Canvas& cv, const PadScreen& ps, const std::vector<std::string>& devices,
              const rd::PadPreview& pad) {
    cv.fill(0, 0, kW, kContentH, kBg);
    cv.fill(0, 0, kW, 56, kHeader);
    cv.text(24, 12, "MANETTE", 4, kWhite);
    const std::vector<rd::InputRow> rows = ps.input.rows(devices);
    const std::string editing = "TOUCHES DE: " + device_name(rows.front().value);
    cv.text(kW - 24 - width_of(rd::fit_text(editing, 480, 2), 2), 22, rd::fit_text(editing, 480, 2),
            2, kGrey);

    // The pad, with the control being changed ringed and those held lit. On a light card, like the
    // VMU's shell: the photographs were cut out against white, and a dark ground shows the fringe.
    constexpr int lx = 24, ly = 64, lw = 376, lh = 376;
    constexpr std::uint32_t kCard = rgb(230, 231, 238), kCardDark = rgb(196, 199, 212);
    cv.fill(lx, ly, lw, lh, kCard);
    rd::PadControl selected{};
    const bool has_selected = ps.input.selected_control(selected);
    auto value = [&](rd::PadControl c) { return pad.value[static_cast<unsigned>(c)]; };
    auto active = [&](rd::PadControl c) {
        return pad.held[static_cast<unsigned>(c)] || value(c) > 0.15f;
    };
    auto mark = [&](const rd::Image& img, int px, int py, const rd::Hotspot& spot,
                    rd::PadControl c) {
        if (!spot.set)
            return;
        const int x = px + static_cast<int>(spot.x * static_cast<float>(img.width));
        const int y = py + static_cast<int>(spot.y * static_cast<float>(img.height));
        const int r = std::max(3, static_cast<int>(spot.r * static_cast<float>(img.width)));
        if (active(c))
            rd::fill_disc(cv.px.data(), kW, kW, kH, x, y, r, 0xC030B8ECu);  // gold, see-through
        if (has_selected && selected == c)
            rd::draw_ring(cv.px.data(), kW, kW, kH, x, y, r + 4, 3, kFocus);
    };
    if (!ps.front.empty()) {
        const int fx = lx + (lw - static_cast<int>(ps.front.width)) / 2, fy = ly + 14;
        rd::blit(cv.px.data(), kW, kW, kH, fx, fy, ps.front);
        for (unsigned i = 0; i < rd::kPadControlCount; ++i)
            mark(ps.front, fx, fy, ps.front_spots[i], static_cast<rd::PadControl>(i));
        // Where the stick is, as a dot between its four marks.
        const auto& f = ps.front_spots;
        const rd::Hotspot& up = f[static_cast<unsigned>(rd::PadControl::StickUp)];
        const rd::Hotspot& dn = f[static_cast<unsigned>(rd::PadControl::StickDown)];
        const rd::Hotspot& lt = f[static_cast<unsigned>(rd::PadControl::StickLeft)];
        const rd::Hotspot& rt = f[static_cast<unsigned>(rd::PadControl::StickRight)];
        if (up.set && dn.set && lt.set && rt.set) {
            const float fw = static_cast<float>(ps.front.width),
                        fh = static_cast<float>(ps.front.height);
            const float sx = value(rd::PadControl::StickRight) - value(rd::PadControl::StickLeft);
            const float sy = value(rd::PadControl::StickDown) - value(rd::PadControl::StickUp);
            const int dx =
                fx + static_cast<int>((lt.x + rt.x) / 2 * fw + sx * (rt.x - lt.x) / 2 * fw);
            const int dy =
                fy + static_cast<int>((up.y + dn.y) / 2 * fh + sy * (dn.y - up.y) / 2 * fh);
            rd::fill_disc(cv.px.data(), kW, kW, kH, dx, dy, 5, kFocus);
            rd::draw_ring(cv.px.data(), kW, kW, kH, dx, dy, 5, 1, kInk);
        }
    } else {
        cv.text(lx + 16, ly + 16, "PAS D IMAGE DE MANETTE", 2, kInk);
        cv.text(lx + 16, ly + 40, "(DOSSIER ASSET/UI)", 2, kInk);
    }
    // The triggers: the underside with their marks, and how far each is pressed.
    const int by = ly + lh - 112;
    if (!ps.back.empty()) {
        const int bx = lx + (lw - static_cast<int>(ps.back.width)) / 2;
        rd::blit(cv.px.data(), kW, kW, kH, bx, by, ps.back);
        mark(ps.back, bx, by, ps.back_spots[static_cast<unsigned>(rd::PadControl::LeftTrigger)],
             rd::PadControl::LeftTrigger);
        mark(ps.back, bx, by, ps.back_spots[static_cast<unsigned>(rd::PadControl::RightTrigger)],
             rd::PadControl::RightTrigger);
    }
    auto gauge = [&](int x, const char* name, rd::PadControl c) {
        const int gy = ly + lh - 30, gw = lw / 2 - 64;
        cv.text(x, gy, name, 2, kInk);
        cv.fill(x + 24, gy + 2, gw, 10, kCardDark);
        cv.fill(x + 24, gy + 2,
                static_cast<int>(static_cast<float>(gw) * std::clamp(value(c), 0.0f, 1.0f)), 10,
                kFocus);
    };
    gauge(lx + 16, "L", rd::PadControl::LeftTrigger);
    gauge(lx + lw / 2 + 8, "R", rd::PadControl::RightTrigger);

    // The rows: the settings, the controls, then reset and back.
    constexpr int rx = 416, ry = 64, rw = kW - 24 - rx, rh = 376;
    cv.fill(rx, ry, rw, rh, kPanel);
    const int x = rx + 16, w = rw - 32;
    int y = ry + 9;
    constexpr int line = 17;
    const rd::InputRow* current = nullptr;
    for (const rd::InputRow& r : rows) {
        using K = rd::InputRow::Kind;
        if ((r.kind == K::Binding && r.control == rd::PadControl::A) || r.kind == K::Reset) {
            cv.fill(x, y + 2, w, 1, kButton);  // a rule between the three groups
            y += 6;
        }
        std::string name, shown;
        switch (r.kind) {
            case K::Device:
                name = "TOUCHES DE";
                shown = "< " + device_name(r.value) + " >";
                break;
            case K::Deadzone:
                name = "ZONE MORTE";
                shown = "< " + std::to_string(r.percent) + "% >";
                break;
            case K::Ramp:
                name = "GACHETTES PROGRESSIVES";
                shown = r.on ? "< OUI >" : "< NON >";
                break;
            case K::Binding:
                name = pad_name(r.control);
                shown = r.waiting ? "APPUIE SUR UNE TOUCHE" : input_name(r.value);
                break;
            case K::Reset:
                name = "REMETTRE LES TOUCHES D ORIGINE";
                break;
            case K::Close:
                name = "RETOUR AUX JEUX";
                break;
        }
        if (r.selected) {
            current = &r;
            cv.fill(x - 8, y - 1, w + 16, line, kFocus);
        }
        cv.text(x, y, name, 2, r.selected ? kInk : kWhite);
        if (!shown.empty()) {
            const int room = w - width_of(name, 2) - 16;
            const std::string fitted = rd::fit_text(shown, room, 2);
            cv.text(x + w - width_of(fitted, 2), y, fitted, 2,
                    r.selected ? kInk : (r.waiting ? kWarn : kLight));
        }
        y += line;
    }

    std::string help = "HAUT/BAS: CHOISIR  LB/RB OU Q/W: MENU  B: RETOUR AUX JEUX";
    if (current) {
        using K = rd::InputRow::Kind;
        if (current->waiting)
            help = "APPUIE SUR LA TOUCHE OU LE BOUTON VOULU  ECHAP: ANNULER";
        else if (current->kind == K::Device && devices.size() <= 1)
            help = "AUCUNE MANETTE TROUVEE: ALLUME-LA OU BRANCHE-LA";
        else if (current->kind == K::Device || current->kind == K::Deadzone ||
                 current->kind == K::Ramp)
            help = "HAUT/BAS: CHOISIR  GAUCHE/DROITE: CHANGER  LB/RB: MENU  B: RETOUR";
        else if (current->kind == K::Binding)
            help = "HAUT/BAS: CHOISIR  A: CHANGER LA TOUCHE  LB/RB: MENU  B: RETOUR";
        else if (current->kind == K::Reset)
            help = "A: REMETTRE LES TOUCHES D ORIGINE DE CET APPAREIL  B: RETOUR";
        else
            help = "A: RETOUR AUX JEUX";
    }
    cv.text(24, kHelpY, rd::fit_text(help, kW - 48, 2), 2, kGrey);
}

// --- starting a game -------------------------------------------------------------------------

// Starts the game with its cards and waits for it with the launcher hidden. The game's output
// goes to <logs>/<id>.log, the way the batch files did it.
void run_game(rd::vk::Window& w, const Options& o, const Game& g, State& s) {
    std::error_code ec;
    fs::create_directories(o.saves, ec);
    fs::create_directories(o.logs, ec);
    const Slots cards = s.ports;
    std::vector<std::string> args;
    if (!g.wince.empty()) {
        // A Windows CE title: dream-wince runs the disc's own executable, DLLs, DirectDraw and
        // DirectSound; an import it lacks stops the run only if it is called. One card, in A1.
        args = {utf8(g.exe),      "--run",    utf8(g.wince), "--lazy",
                "--real-directx", "--window", "--disc",      utf8(g.image)};
        if (!cards[0].empty()) {
            args.push_back("--vmu");
            args.push_back(utf8(o.saves / cards[0]));
        }
    } else {
        args = {utf8(g.exe), "--config", utf8(g.config), "--window", "--vmu-dir", utf8(o.saves)};
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
    }
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

void ask_for_disc(rd::vk::Window& w, Import& imp, const Options& o, std::string& message) {
    if (const std::string why = import_blocker(o); !why.empty()) {
        message = why;
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
    if (!g.wince.empty())
        return;  // a Windows CE title has no translation to improve
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
    // A game added goes to the library to be played; a failed one leaves the adding screen up. A
    // game built again after a session stays where it was (it still plays as it was).
    if (!imp.update)
        s.action = ok ? kPlay : kAdd;
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

// --- the memory cards screen -----------------------------------------------------------------
//
// The player's cards as a VMU shows them: the card drawn on the left with the selected save on its
// screen, its saves listed on the right. Left and right go from card to card, and the place after
// the last makes a new one; up and down go from save to save; A copies a save to another card or
// deletes it; B leaves. The card's own buttons light as the matching ones are pressed.

using dream::maple::VmuEdit;

// What A offers on a card: copying the save to each other card, deleting it, and plugging the card
// into the pad (or taking it out).
struct CardAction {
    enum class Kind { Copy, Delete, Plug, Unplug, Cancel };
    Kind kind = Kind::Cancel;
    std::string target;  // Copy: the card it goes to
    unsigned port = 0;   // Plug: 0 for A1, 1 for A2
    std::string label;
};

struct CardsScreen {
    enum class Mode { Browse, Actions, Confirm };
    bool open = false;
    Mode mode = Mode::Browse;
    std::vector<std::string> files;  // the cards; the index files.size() is the place for a new one
    int card = 0, save = 0;
    std::vector<std::uint8_t> image;
    dream::maple::VmuContents contents;
    std::vector<CardAction> menu;  // what A offered
    std::string next;              // the file a new card would get
    int choice = 0;
    std::string message;
    rd::VmuPressed lit{};
    std::array<std::uint64_t, static_cast<unsigned>(rd::VmuButton::Count)> lit_until{};

    bool on_new() const { return card >= static_cast<int>(files.size()); }
    int saves() const { return static_cast<int>(contents.files.size()); }
    const dream::maple::VmuFile* file() const {
        return on_new() || contents.files.empty() ? nullptr
                                                  : &contents.files[static_cast<std::size_t>(save)];
    }
    std::string title() const {
        return on_new() ? "NOUVELLE VMU" : rd::card_label(files[static_cast<std::size_t>(card)]);
    }
    std::string here() const {
        return on_new() ? std::string{} : files[static_cast<std::size_t>(card)];
    }
    // What A offers on the card shown, with the pad's slots as they are.
    void build_menu(const Slots& ports) {
        using K = CardAction::Kind;
        menu.clear();
        const std::string me = here();
        if (file()) {
            for (const std::string& t : files)
                if (t != me)
                    menu.push_back({K::Copy, t, 0, "COPIER VERS " + rd::card_label(t)});
            menu.push_back({K::Delete, {}, 0, "SUPPRIMER LA SAUVEGARDE"});
        }
        for (unsigned p = 0; p < 2; ++p)
            if (ports[p] != me)
                menu.push_back(
                    {K::Plug, {}, p, p == 0 ? "BRANCHER EN A1 (TOUS LES JEUX)" : "BRANCHER EN A2"});
        if (ports[0] == me || ports[1] == me)
            menu.push_back({K::Unplug, {}, 0, "DEBRANCHER CETTE VMU"});
        menu.push_back({K::Cancel, {}, 0, "ANNULER"});
        choice = 0;
    }

    void load(const Options& o) {
        image = on_new()
                    ? std::vector<std::uint8_t>{}
                    : dream::launcher::read_card(o.saves / files[static_cast<std::size_t>(card)]);
        contents = dream::maple::read_vmu(image);
        save = std::clamp(save, 0, std::max(0, saves() - 1));
    }
    // The cards as they are on disk now, showing `file`.
    void show(const Options& o, const std::string& file) {
        files = dream::launcher::list_cards(o.saves);
        next = dream::launcher::next_card(o.saves);
        card = 0;
        for (std::size_t i = 0; i < files.size(); ++i)
            if (files[i] == file)
                card = static_cast<int>(i);
        load(o);
    }
    void press(rd::VmuButton b) { lit_until[static_cast<unsigned>(b)] = SDL_GetTicks() + 160; }
    void tick() {
        const std::uint64_t now = SDL_GetTicks();
        for (std::size_t i = 0; i < lit.size(); ++i) lit[i] = lit_until[i] > now;
    }
};

std::string edit_message(VmuEdit r, const std::string& card) {
    switch (r) {
        case VmuEdit::Done:
            return {};
        case VmuEdit::Exists:
            return card + " A DEJA UNE SAUVEGARDE DE CE JEU";
        case VmuEdit::NoSpace:
            return "PAS ASSEZ DE BLOCS LIBRES SUR " + card;
        case VmuEdit::DirectoryFull:
            return card + " NE PEUT PAS GARDER PLUS DE FICHIERS";
        case VmuEdit::Protected:
            return "LE JEU INTERDIT LA COPIE DE CETTE SAUVEGARDE";
        case VmuEdit::MiniGame:
            return "LES MINI-JEUX VMU NE SE COPIENT PAS ENCORE";
        case VmuEdit::NotACard:
        case VmuEdit::NotFound:
            break;
    }
    return "CARTE ILLISIBLE: " + card;
}

void open_cards(CardsScreen& cs, const Options& o, const std::string& file) {
    cs.open = true;
    cs.mode = CardsScreen::Mode::Browse;
    cs.message.clear();
    cs.save = 0;
    cs.show(o, file);
}

void pump_cards(rd::vk::Window& w, CardsScreen& cs, State& s, const Options& o) {
    using B = rd::VmuButton;
    using Mode = CardsScreen::Mode;
    const bool up = w.pressed(Control::Up), down = w.pressed(Control::Down),
               left = w.pressed(Control::Left), right = w.pressed(Control::Right),
               a = w.pressed(Control::A), b = w.pressed(Control::Back),
               mode = w.pressed(Control::Start);
    for (const auto& [held, button] :
         {std::pair{up, B::Up}, std::pair{down, B::Down}, std::pair{left, B::Left},
          std::pair{right, B::Right}, std::pair{a, B::A}, std::pair{b, B::B},
          std::pair{mode, B::Mode}})
        if (held)
            cs.press(button);
    const dream::maple::VmuFile* f = cs.file();
    const std::string here = cs.here();
    switch (cs.mode) {
        case Mode::Browse: {
            if (b) {
                cs.open = false;
                return;
            }
            if (up || down) {
                cs.save = std::clamp(cs.save + (down ? 1 : -1), 0, std::max(0, cs.saves() - 1));
                cs.message.clear();
            }
            const int places = static_cast<int>(cs.files.size()) + 1;
            if (left || right || mode) {
                // MODE goes round the cards, as it goes round a VMU's modes.
                cs.card = mode ? (cs.card + 1) % places
                               : std::clamp(cs.card + (right ? 1 : -1), 0, places - 1);
                cs.save = 0;
                cs.load(o);
                cs.message.clear();
            }
            if (!a)
                break;
            if (cs.on_new()) {
                const std::string made = dream::launcher::next_card(o.saves);
                if (dream::launcher::create_card(o.saves / made)) {
                    cs.show(o, made);
                    cs.message = rd::card_label(made) + " CREEE";
                } else {
                    cs.message = "IMPOSSIBLE DE CREER " + rd::card_label(made);
                }
            } else {
                cs.build_menu(s.ports);
                cs.mode = Mode::Actions;
            }
            break;
        }
        case Mode::Actions: {
            const int n = static_cast<int>(cs.menu.size());
            if (b) {
                cs.mode = Mode::Browse;
                break;
            }
            if (up)
                cs.choice = std::max(0, cs.choice - 1);
            if (down)
                cs.choice = std::min(n - 1, cs.choice + 1);
            if (!a || n == 0)
                break;
            const CardAction act = cs.menu[static_cast<std::size_t>(cs.choice)];
            cs.mode = Mode::Browse;
            switch (act.kind) {
                case CardAction::Kind::Copy: {
                    if (!f)
                        break;
                    std::vector<std::uint8_t> dst =
                        dream::launcher::read_card(o.saves / act.target);
                    const VmuEdit r = dst.empty()
                                          ? VmuEdit::NotACard
                                          : dream::maple::copy_vmu_file(cs.image, f->name, dst);
                    if (r == VmuEdit::Done &&
                        dream::launcher::write_card(o.saves / act.target, dst))
                        cs.message = "COPIEE DANS " + rd::card_label(act.target);
                    else
                        cs.message = edit_message(r == VmuEdit::Done ? VmuEdit::NotACard : r,
                                                  rd::card_label(act.target));
                    break;
                }
                case CardAction::Kind::Delete:
                    cs.mode = Mode::Confirm;
                    break;
                case CardAction::Kind::Plug:
                    // One card in one slot: plugging it into the other takes it out of this one.
                    s.ports[act.port] = here;
                    if (s.ports[1 - act.port] == here)
                        s.ports[1 - act.port].clear();
                    save_ports(s, o);
                    cs.message = cs.title() +
                                 (act.port == 0 ? " BRANCHEE EN A1" : " BRANCHEE EN A2") +
                                 ": TOUS LES JEUX LA TROUVENT";
                    break;
                case CardAction::Kind::Unplug:
                    for (std::string& p : s.ports)
                        if (p == here)
                            p.clear();
                    save_ports(s, o);
                    cs.message = cs.title() + " DEBRANCHEE";
                    break;
                case CardAction::Kind::Cancel:
                    break;
            }
            break;
        }
        case Mode::Confirm: {
            if (b) {
                cs.mode = Mode::Browse;
                break;
            }
            if (!a || !f)
                break;
            std::vector<std::uint8_t> img = cs.image;
            if (dream::maple::delete_vmu_file(img, f->name) == VmuEdit::Done &&
                dream::launcher::write_card(o.saves / here, img)) {
                cs.message = "SAUVEGARDE SUPPRIMEE. ANCIENNE VERSION: " + upper(here) + ".BAK";
                cs.load(o);
            } else {
                cs.message = "SUPPRESSION IMPOSSIBLE";
            }
            cs.mode = Mode::Browse;
            break;
        }
    }
}

// A save's icon, its transparent pixels left as they are.
void blit_icon(Canvas& cv, int x, int y, const std::vector<std::uint32_t>& icon) {
    if (icon.size() != dream::maple::kVmuIconSize * dream::maple::kVmuIconSize)
        return;
    for (int iy = 0; iy < 32; ++iy)
        for (int ix = 0; ix < 32; ++ix) {
            const std::uint32_t c = icon[static_cast<std::size_t>(iy * 32 + ix)];
            if ((c >> 24) >= 128 && x + ix >= 0 && x + ix < kW && y + iy >= 0 && y + iy < kH)
                cv.px[static_cast<std::size_t>(y + iy) * kW + static_cast<std::size_t>(x + ix)] =
                    c | 0xFF000000u;
        }
}

void draw_cards(Canvas& cv, const CardsScreen& cs, const Slots& ports) {
    using Mode = CardsScreen::Mode;
    cv.fill(0, 0, kW, kH, kBg);
    cv.fill(0, 0, kW, 56, kHeader);
    cv.text(24, 12, "CARTES VMU", 4, kWhite);
    const std::string count =
        std::to_string(cs.files.size()) + (cs.files.size() == 1 ? " CARTE" : " CARTES");
    cv.text(kW - 24 - width_of(count, 2), 22, count, 2, kGrey);

    // The card, with the selected save on its screen.
    const rd::VmuLcd lcd = rd::vmu_file_screen(cs.file(), static_cast<unsigned>(cs.save),
                                               static_cast<unsigned>(cs.saves()), cs.on_new());
    constexpr int vmu_h = 384;
    rd::draw_vmu(cv.px.data(), kW, kW, kContentH, 24 + (320 - rd::vmu_width(vmu_h)) / 2, 58, vmu_h,
                 lcd, cs.on_new() ? "+" : cs.title(), cs.lit);

    // Its saves.
    constexpr int px = 360, py = 72, pw = kW - 24 - px, ph = 360;
    cv.fill(px, py, pw, ph, kPanel);
    const int x = px + 16, w = pw - 32;
    int y = py + 14;
    const std::string where =
        cs.on_new() ? "+" : std::to_string(cs.card + 1) + " / " + std::to_string(cs.files.size());
    const std::string arrows = "<  " + where + "  >";
    cv.text(x, y, rd::fit_text(cs.title(), w - width_of(arrows, 2) - 16, 3), 3, kWhite);
    cv.text(x + w - width_of(arrows, 2), y + 4, arrows, 2, kGrey);
    y += 40;
    if (!cs.on_new()) {
        // Whether the pad has it: what every game finds.
        const std::string me = cs.here();
        const int port = ports[0] == me ? 0 : ports[1] == me ? 1 : -1;
        const std::string_view badge = port == 0   ? "BRANCHEE EN A1"
                                       : port == 1 ? "BRANCHEE EN A2"
                                                   : "PAS BRANCHEE";
        cv.fill(x, y, width_of(badge, 2) + 16, 24, port >= 0 ? kGood : kButton);
        cv.text(x + 8, y + 5, badge, 2, port >= 0 ? kInk : kLight);
        y += 36;
    }
    if (cs.on_new()) {
        cv.text(x, y, "UNE VMU VIDE DE PLUS, POUR TOUS LES JEUX.", 2, kLight);
        cv.text(x, y + 28, "A: CREER " + rd::card_label(cs.next), 2, kFocus);
    } else if (!cs.contents.formatted) {
        cv.text(x, y, "CETTE CARTE N EST PAS FORMATEE.", 2, kWarn);
    } else {
        const unsigned total = cs.contents.user_blocks, free = cs.contents.free_blocks;
        cv.text(x, y, "BLOCS LIBRES", 2, kGrey);
        const std::string blocks = std::to_string(free) + " / " + std::to_string(total);
        cv.text(x + w - width_of(blocks, 2), y, blocks, 2, kLight);
        y += 22;
        cv.fill(x, y, w, 10, kButton);
        if (total > 0)
            cv.fill(x, y, static_cast<int>(static_cast<long long>(w) * (total - free) / total), 10,
                    free < total / 10 ? kWarn : kGood);
        y += 22;
        if (dream::launcher::card_number(cs.files[static_cast<std::size_t>(cs.card)]) == 0) {
            cv.text(x, y, "ANCIENNE CARTE D UN SEUL JEU", 2, kGrey);
            y += 24;
        }
        if (cs.contents.files.empty())
            cv.text(x, y + 8, "AUCUNE SAUVEGARDE SUR CETTE VMU.", 2, kGrey);
        constexpr int rowh = 50;
        const int rows = (py + ph - 8 - y) / rowh;
        const int first = std::max(0, cs.save - rows + 1);
        for (int i = 0; i < rows && first + i < cs.saves(); ++i) {
            const dream::maple::VmuFile& f = cs.contents.files[static_cast<std::size_t>(first + i)];
            const int ry = y + i * rowh;
            if (first + i == cs.save)
                cv.fill(x - 8, ry, w + 16, rowh - 4, kSelect);
            blit_icon(cv, x, ry + 7, f.icon);
            const std::string desc = f.description.empty() ? f.name : f.description;
            cv.text(x + 44, ry + 6, rd::fit_text(desc, w - 44, 2), 2, kWhite);
            std::string info = f.name + "  " + std::to_string(f.blocks) + " BLOCS";
            if (f.no_copy)
                info += "  PROTEGEE";
            cv.text(x + 44, ry + 26, rd::fit_text(info, w - 44, 2), 2, kGrey);
        }
    }

    // What A does with the card and its save, and the question before a delete.
    if (cs.mode != Mode::Browse) {
        std::vector<std::string> acts;
        for (const CardAction& a : cs.menu) acts.push_back(a.label);
        const bool confirm = cs.mode == Mode::Confirm && cs.file();
        // At most eight lines, scrolled to keep the choice in view.
        constexpr int kMaxLines = 8;
        const int lines = confirm ? 3 : std::min(kMaxLines, static_cast<int>(acts.size()));
        const int top = confirm ? 0 : std::max(0, cs.choice - kMaxLines + 1);
        const int bw = pw - 80, bh = 64 + lines * 30, bx = px + 40, by = py + 60;
        cv.fill(bx - 3, by - 3, bw + 6, bh + 6, kFocus);
        cv.fill(bx, by, bw, bh, kHeader);
        const dream::maple::VmuFile* f = cs.file();
        const std::string desc = !f                       ? cs.title()
                                 : f->description.empty() ? f->name
                                                          : f->description;
        cv.text(bx + 16, by + 14,
                rd::fit_text(confirm ? "SUPPRIMER CETTE SAUVEGARDE ?" : desc, bw - 32, 2), 2,
                kWhite);
        if (confirm) {
            cv.text(bx + 16, by + 48, rd::fit_text(desc + " (" + cs.title() + ")", bw - 32, 2), 2,
                    kLight);
            cv.text(bx + 16, by + 84, "A: OUI, SUPPRIMER    B: NON", 2, kWarn);
        } else {
            for (int i = 0; i < lines; ++i) {
                const int ly = by + 48 + i * 30;
                const bool sel = top + i == cs.choice;
                if (sel)
                    cv.fill(bx + 8, ly - 6, bw - 16, 28, kFocus);
                cv.text(bx + 16, ly,
                        rd::fit_text(acts[static_cast<std::size_t>(top + i)], bw - 32, 2), 2,
                        sel ? kInk : kWhite);
            }
        }
    }

    const std::string_view help =
        cs.mode == Mode::Browse
            ? "HAUT/BAS: SAUVEGARDE  GAUCHE/DROITE: VMU  A: ACTIONS  LB/RB: MENU  B: RETOUR"
            : "HAUT/BAS: CHOISIR  A: VALIDER  B: ANNULER";
    if (cs.message.empty())
        cv.text(24, kHelpY, rd::fit_text(help, kW - 48, 2), 2, kGrey);
    else
        cv.text(24, kHelpY, rd::fit_text(cs.message, kW - 48, 2), 2, kWarn);
}

// --- the adding screen -----------------------------------------------------------------------
//
// The dumps in the player's folder (--discs), each marked with whether the library has it; A adds
// the one chosen, and the last row opens the file dialog for one kept anywhere else.

struct AddScreen {
    std::vector<dream::launcher::Disc> discs;
    int sel = 0;
    bool stale = true;  // read the folder again before showing it
    std::string message;

    bool on_other() const { return sel >= static_cast<int>(discs.size()); }
    void refresh(const Options& o, const std::vector<Game>& games) {
        discs = o.discs.empty() ? std::vector<dream::launcher::Disc>{}
                                : dream::launcher::find_discs(o.discs, games);
        sel = std::clamp(sel, 0, static_cast<int>(discs.size()));
        stale = false;
    }
};

void draw_add(Canvas& cv, const AddScreen& as, const Options& o) {
    cv.fill(0, 0, kW, kContentH, kBg);
    cv.fill(0, 0, kW, 56, kHeader);
    cv.text(24, 12, "AJOUTER UN JEU", 4, kWhite);
    const std::string folder = o.discs.empty() ? std::string("") : upper(utf8(o.discs.filename()));
    const std::string count = std::to_string(as.discs.size()) +
                              (as.discs.size() == 1 ? " DISQUE" : " DISQUES") +
                              (folder.empty() ? "" : " DANS " + folder);
    cv.text(kW - 24 - width_of(count, 2), 22, rd::fit_text(count, 420, 2), 2, kGrey);

    // The dumps, green when the library has them, then the way to any other file.
    constexpr int lx = 24, ly = 72, lw = 376, rowh = 36, rows = 10;
    cv.fill(lx, ly, lw, rows * rowh, kPanel);
    const int n = static_cast<int>(as.discs.size()) + 1;
    const int first = std::max(0, as.sel - rows + 1);
    for (int i = 0; i < rows && first + i < n; ++i) {
        const int k = first + i, y = ly + i * rowh;
        if (k == as.sel)
            cv.fill(lx, y, lw, rowh, kSelect);
        if (k < static_cast<int>(as.discs.size())) {
            const dream::launcher::Disc& d = as.discs[static_cast<std::size_t>(k)];
            cv.fill(lx + 12, y + 12, 12, 12, d.game_id.empty() ? kWarn : kGood);
            cv.text(lx + 36, y + 10, rd::fit_text(d.name, lw - 48, 2), 2, kWhite);
        } else {
            cv.text(lx + 36, y + 10, "AUTRE DISQUE...", 2, kLight);
        }
    }

    // What A does with it.
    constexpr int dx = 416, dy = ly, dw = kW - 24 - dx, dh = rows * rowh;
    cv.fill(dx, dy, dw, dh, kPanel);
    const int x = dx + 16, w = dw - 32;
    int y = dy + 16;
    auto para = [&](std::string_view text, std::uint32_t c) {
        for (const std::string& l : wrap(text, w, 2)) {
            cv.text(x, y, l, 2, c);
            y += 20;
        }
        y += 12;
    };
    if (as.on_other()) {
        cv.text(x, y, "AUTRE DISQUE", 3, kWhite);
        y += 44;
        para("A: CHOISIR UN FICHIER .GDI, .CDI OU .CHD AILLEURS SUR LE PC.", kLight);
        if (as.discs.empty())
            para(folder.empty() ? "AUCUN DOSSIER DE DISQUES N EST INDIQUE AU LANCEUR."
                                : "AUCUN DISQUE DANS " + folder + ".",
                 kGrey);
    } else {
        const dream::launcher::Disc& d = as.discs[static_cast<std::size_t>(as.sel)];
        for (const std::string& l : wrap(d.name, w, 2)) {
            cv.text(x, y, l, 2, kWhite);
            y += 20;
        }
        y += 16;
        if (d.game_id.empty()) {
            const std::string_view label = "PAS ENCORE AJOUTE";
            cv.fill(x, y, width_of(label, 2) + 16, 28, kWarn);
            cv.text(x + 8, y + 7, label, 2, kInk);
            y += 48;
            para(
                "A: L AJOUTER. LE LANCEUR LIT LE DISQUE, COMPILE LE JEU, L ESSAIE SANS FENETRE ET "
                "REGLE SA CONFIGURATION TOUT SEUL. COMPTER QUELQUES MINUTES.",
                kLight);
        } else {
            const std::string_view label = "DANS LA BIBLIOTHEQUE";
            cv.fill(x, y, width_of(label, 2) + 16, 28, kGood);
            cv.text(x + 8, y + 7, label, 2, kInk);
            y += 48;
            para(d.game_title, kWhite);
            para("A: LE RECOMPILER ET LE REESSAYER. SA NOTE ET SES SAUVEGARDES NE CHANGENT PAS.",
                 kLight);
        }
    }

    if (as.message.empty())
        cv.text(24, kHelpY, "HAUT/BAS: DISQUE  A: AJOUTER  LB/RB OU Q/W: MENU  B: RETOUR AUX JEUX",
                2, kGrey);
    else
        cv.text(24, kHelpY, rd::fit_text(as.message, kW - 48, 2), 2, kWarn);
}

// Returns the screen to go to, or -1.
int pump_add(rd::vk::Window& w, AddScreen& as, Import& imp, const Options& o) {
    const int n = static_cast<int>(as.discs.size()) + 1;
    if (w.pressed(Control::Up) || w.pressed(Control::Down)) {
        as.sel = std::clamp(as.sel + (w.pressed(Control::Down) ? 1 : -1), 0, n - 1);
        as.message.clear();
    }
    if (w.pressed(Control::Back))
        return kPlay;
    if (!w.pressed(Control::A) && !w.pressed(Control::Start))
        return -1;
    if (as.on_other()) {
        ask_for_disc(w, imp, o, as.message);
    } else if (const std::string why = import_blocker(o); !why.empty()) {
        as.message = why;
    } else {
        as.message.clear();
        start_import(imp, o, utf8(as.discs[static_cast<std::size_t>(as.sel)].image));
    }
    return -1;
}

// --- the library's own keys ------------------------------------------------------------------

// Returns the screen to go to, or -1.
int pump_library(rd::vk::Window& w, State& s, Import& imp, const Options& o) {
    const int n = static_cast<int>(s.games.size());
    if ((w.pressed(Control::Up) || w.pressed(Control::Down)) && n > 0) {
        s.sel = std::clamp(s.sel + (w.pressed(Control::Down) ? 1 : -1), 0, n - 1);
        s.message.clear();
    }
    if (w.pressed(Control::Menu))
        return kPad;
    if (w.pressed(Control::Back))
        return kQuit;
    if (!w.pressed(Control::A) && !w.pressed(Control::Start))
        return -1;
    const Game* g = s.game();
    if (!g)
        return -1;
    if (!g->built)
        s.message = "PAS ENCORE COMPILE: L AJOUTER AVEC AJOUTER UN JEU";
    else if (!g->disc)
        s.message = "DISQUE ABSENT: VOIR GAMES/" + upper(g->id) + "/DISC";
    else {
        run_game(w, o, *g, s);
        after_play(imp, s, o, *g);
    }
    return -1;
}

int pump_quit(rd::vk::Window& w, bool& running) {
    if (w.pressed(Control::A) || w.pressed(Control::Start))
        running = false;
    return w.pressed(Control::Back) ? kPlay : -1;
}

// --- going from screen to screen -------------------------------------------------------------

struct Screens {
    State& s;
    PadScreen& ps;
    CardsScreen& cs;
    AddScreen& as;
    const Options& o;
};

// The bar: a click on it, the shoulder buttons (Q/W), and left and right on the screens that have
// no other use for them. -1 for none.
int bar_request(rd::vk::Window& w, const Screens& sc) {
    const int here = sc.s.action;
    int x = 0, y = 0;
    if (w.take_click(kW, kH, x, y))
        if (const int hit = bar_hit(x, y); hit >= 0)
            return hit;
    if (here == kPad && sc.ps.input.awaiting())
        return -1;  // a key is being bound: every key belongs to that
    bool prev = w.pressed(Control::PrevTab), next = w.pressed(Control::NextTab);
    if (here == kPlay || here == kAdd || here == kQuit) {
        prev = prev || w.pressed(Control::Left);
        next = next || w.pressed(Control::Right);
    }
    if (next)
        return (here + 1) % kActions;
    if (prev)
        return (here + kActions - 1) % kActions;
    return -1;
}

// The card the cards screen opens on: the one in the pad's first slot.
std::string plugged_card(const State& s) {
    return s.ports[0].empty() ? std::string(dream::launcher::kFirstCard) : s.ports[0];
}

// Leaves the screen shown and opens `to` in its place.
void switch_screen(rd::vk::Window& w, const Screens& sc, int to) {
    State& s = sc.s;
    if (to < 0 || to == s.action)
        return;
    if (s.action == kPad && sc.ps.is_open())
        sc.ps.close(w);  // writes the bindings
    if (s.action == kCards)
        sc.cs.open = false;
    s.action = to;
    if (to == kPad)
        sc.ps.open(w);
    if (to == kCards)
        open_cards(sc.cs, sc.o, plugged_card(s));
    if (to == kAdd) {
        sc.as.stale = true;
        sc.as.message.clear();
    }
}

void draw_screen(Canvas& cv, const Screens& sc, const std::vector<std::string>& devices,
                 const rd::PadPreview& pad) {
    switch (sc.s.action) {
        case kPlay:
            draw(cv, sc.s);
            break;
        case kAdd:
            draw_add(cv, sc.as, sc.o);
            break;
        case kCards:
            draw_cards(cv, sc.cs, sc.s.ports);
            break;
        case kPad:
            draw_pad(cv, sc.ps, devices, pad);
            break;
        default:
            draw_quit(cv);
            break;
    }
    draw_bar(cv, sc.s.action);
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
                "  --saves      memory cards, vmu-<n>.bin, shared by every game (default: user "
                "data)\n"
                "  --logs       each game's output, <id>.log (default: user data)\n"
                "  --ui-assets  pictures of the pad for the MANETTE screen\n"
                "  --dcdisc     the disc tool, for adding a game (default: the checkout's .venv)\n"
                "  --discs      the folder of disc images the AJOUTER UN JEU screen lists\n"
                "  --screenshot draw one frame to a PPM file and exit; --tab play, add, cards,\n"
                "               cards-menu, cards-delete, cards-new, pad or quit picks the "
                "screen,\n"
                "               --tab import or update draws adding or rebuilding a game\n");
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
    load_ports(s, o);
    rescan(s, o);
    // The first time with shared cards: the saves of the one-card-per-game files go onto VMU 1.
    const dream::launcher::Gathered gathered = dream::launcher::gather_saves(o.saves);
    if (gathered.made)
        s.message = gathered.copied == 0 ? "VMU 1 CREEE, POUR TOUS LES JEUX"
                                         : "VMU 1 CREEE: " + std::to_string(gathered.copied) +
                                               " SAUVEGARDES REGROUPEES, ANCIENNES CARTES MISES "
                                               "DE COTE";
    PadScreen ps;
    ps.load_art(o.ui_assets);
    Canvas cv;
    CardsScreen cs;
    AddScreen as;
    const Screens sc{s, ps, cs, as, o};
    if (!o.screenshot.empty()) {
        // --tab picks the screen: play (the default), add, cards (or cards-menu, cards-delete,
        // cards-new), pad or quit; import and update draw the progress of adding or rebuilding a
        // game over the library.
        s.sel = std::clamp(o.select, 0, std::max(0, static_cast<int>(s.games.size()) - 1));
        const std::string& t = o.tab;
        if (t == "add") {
            s.action = kAdd;
            as.refresh(o, s.games);
        } else if (t.rfind("cards", 0) == 0) {
            s.action = kCards;
            open_cards(cs, o, plugged_card(s));
            if (t == "cards-new")
                cs.card = static_cast<int>(cs.files.size());
            if (t == "cards-menu" || t == "cards-delete") {
                cs.build_menu(s.ports);
                cs.mode =
                    t == "cards-menu" ? CardsScreen::Mode::Actions : CardsScreen::Mode::Confirm;
            }
            cs.lit[static_cast<unsigned>(rd::VmuButton::A)] = t != "cards";
        } else if (t == "pad" || t == "controller") {
            s.action = kPad;
            ps.input.open(ps.load_bindings());
        } else if (t == "quit") {
            s.action = kQuit;
        }
        draw_screen(cv, sc, {"KEYBOARD"}, rd::PadPreview{});
        if (t == "import" || t == "update") {
            const bool update = t == "update";
            draw_import(cv, update,
                        update ? "Tokyo Highway Challenge" : "Demo Disc v1.000 (2000)(PAL)[!].gdi",
                        update ? "TRADUCTION ET COMPILATION" : "TRADUCTION ET COMPILATION (1/3)",
                        update ? "tokyohighwaychallenge" : "demo", 754000, false);
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
    w.set_bindings(ps.load_bindings());
    w.set_escape_quits(false);  // escape goes back a step, as in the games' own settings screen

    Import imp;
    bool running = true;
    while (running && w.poll()) {
        const bool was_busy = imp.busy();
        poll_import(imp, s, o);
        if (was_busy && !imp.busy())
            as.stale = true;  // a game was added: which dumps the library has changes
        if (imp.busy()) {
            if (w.pressed(Control::Back))
                imp.status->cancel = true;
        } else {
            int to = bar_request(w, sc);
            if (to < 0) {
                switch (s.action) {
                    case kPlay:
                        to = pump_library(w, s, imp, o);
                        break;
                    case kAdd:
                        to = pump_add(w, as, imp, o);
                        break;
                    case kCards:
                        pump_cards(w, cs, s, o);
                        if (!cs.open)
                            to = kPlay;
                        break;
                    case kPad:
                        ps.pump(w);
                        if (!ps.is_open())
                            to = kPlay;
                        break;
                    default:
                        to = pump_quit(w, running);
                        break;
                }
            }
            switch_screen(w, sc, to);
        }
        if (s.action == kAdd && as.stale)
            as.refresh(o, s.games);
        if (s.action == kCards)
            cs.tick();
        draw_screen(cv, sc, w.devices(), ps.preview(w));
        if (imp.busy())
            draw_import(cv, imp);
        present(w, p, cv);
    }
    if (ps.is_open())
        ps.close(w);   // the bindings changed on a screen still open are kept
    stop_import(imp);  // closing the window stops an import half done
    vkDeviceWaitIdle(w.context().device());
    p.destroy();
    w.destroy();
    return 0;
}
