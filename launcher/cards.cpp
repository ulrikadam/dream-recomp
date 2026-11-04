#include "cards.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <system_error>

#include "dream/runtime/maple/maple.h"
#include "dream/runtime/maple/vmu_fs.h"

namespace dream::launcher {

namespace fs = std::filesystem;
using dream::maple::VmuEdit;

namespace {

constexpr std::uintmax_t kCardSize = dream::maple::MemoryCard::kImageSize;

std::string file_name(const fs::path& p) {
    const auto s = p.filename().u8string();
    return std::string(s.begin(), s.end());
}

std::string shared_name(unsigned n) {
    return "vmu-" + std::to_string(n) + ".bin";
}

bool save(const fs::path& path, const std::vector<std::uint8_t>& image) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(image.data()),
              static_cast<std::streamsize>(image.size()));
    return static_cast<bool>(out);
}

}  // namespace

unsigned card_number(std::string_view file) {
    if (file.size() <= 8 || file.compare(0, 4, "vmu-") != 0 ||
        file.compare(file.size() - 4, 4, ".bin") != 0)
        return 0;
    const std::string_view digits = file.substr(4, file.size() - 8);
    if (digits.size() > 6 || digits[0] == '0' ||
        digits.find_first_not_of("0123456789") != std::string_view::npos)
        return 0;
    unsigned n = 0;
    for (char c : digits) n = n * 10 + static_cast<unsigned>(c - '0');
    return n;
}

std::vector<std::string> list_cards(const fs::path& saves) {
    std::vector<std::string> shared, other;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(saves, ec)) {
        if (!e.is_regular_file(ec) || e.file_size(ec) != kCardSize ||
            e.path().extension() != ".bin")
            continue;
        const std::string name = file_name(e.path());
        (card_number(name) ? shared : other).push_back(name);
    }
    std::sort(shared.begin(), shared.end(), [](const std::string& a, const std::string& b) {
        return card_number(a) < card_number(b);
    });
    std::sort(other.begin(), other.end());
    shared.insert(shared.end(), other.begin(), other.end());
    return shared;
}

std::string next_card(const fs::path& saves) {
    std::error_code ec;
    for (unsigned n = 1;; ++n)
        if (!fs::exists(saves / shared_name(n), ec))
            return shared_name(n);
}

std::vector<std::uint8_t> read_card(const fs::path& path) {
    std::error_code ec;
    if (fs::file_size(path, ec) != kCardSize || ec)
        return {};
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> image((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    return image.size() == kCardSize ? image : std::vector<std::uint8_t>{};
}

bool write_card(const fs::path& path, const std::vector<std::uint8_t>& image) {
    if (image.size() != kCardSize)
        return false;
    std::error_code ec;
    if (fs::exists(path, ec)) {
        fs::path backup = path;
        backup += ".bak";
        fs::copy_file(path, backup, fs::copy_options::overwrite_existing, ec);
        if (ec)
            return false;
    }
    return save(path, image);
}

bool create_card(const fs::path& path) {
    std::error_code ec;
    if (fs::exists(path, ec))
        return false;
    fs::create_directories(path.parent_path(), ec);
    return save(path, dream::maple::blank_vmu());
}

Gathered gather_saves(const fs::path& saves) {
    Gathered g;
    std::error_code ec;
    if (fs::exists(saves / kFirstCard, ec))
        return g;

    // The older cards, newest first.
    struct Source {
        fs::path path;
        fs::file_time_type time;
    };
    std::vector<Source> sources;
    for (const std::string& name : list_cards(saves))
        if (!card_number(name))
            sources.push_back({saves / name, fs::last_write_time(saves / name, ec)});
    std::stable_sort(sources.begin(), sources.end(),
                     [](const Source& a, const Source& b) { return a.time > b.time; });

    std::string current = kFirstCard;
    std::vector<std::uint8_t> card = dream::maple::blank_vmu();
    std::set<std::string> taken;  // one save of a name, as a title would keep
    fs::create_directories(saves, ec);
    auto finish = [&] {
        std::error_code e;
        if (!fs::exists(saves / current, e) && save(saves / current, card))
            g.cards.push_back(current);
    };
    for (const Source& s : sources) {
        const std::vector<std::uint8_t> image = read_card(s.path);
        for (const dream::maple::VmuFile& f : dream::maple::read_vmu(image).files) {
            if (f.game || taken.count(f.name)) {
                ++g.skipped;
                continue;
            }
            VmuEdit r = dream::maple::copy_vmu_file(image, f.name, card, true);
            if (r == VmuEdit::NoSpace || r == VmuEdit::DirectoryFull) {
                finish();
                current = next_card(saves);
                card = dream::maple::blank_vmu();
                r = dream::maple::copy_vmu_file(image, f.name, card, true);
            }
            if (r == VmuEdit::Done) {
                ++g.copied;
                taken.insert(f.name);
            } else {
                ++g.skipped;
            }
        }
    }
    finish();
    g.made = !g.cards.empty() && g.cards.front() == kFirstCard;
    if (!g.made)
        return g;  // nothing was gathered: the old cards stay in use
    const fs::path away = saves / kOldCards;
    fs::create_directories(away, ec);
    for (const Source& s : sources) {
        std::error_code e;
        const fs::path to = away / s.path.filename();
        if (fs::exists(to, e))
            continue;
        fs::rename(s.path, to, e);
        if (!e)
            ++g.moved;
    }
    return g;
}

}  // namespace dream::launcher
