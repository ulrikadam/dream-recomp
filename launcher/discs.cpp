#include "discs.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace dream::launcher {

namespace fs = std::filesystem;

namespace {

std::string lower_ext(const fs::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

bool is_image(const fs::path& p) {
    const std::string e = lower_ext(p);
    return e == ".gdi" || e == ".cdi" || e == ".chd";
}

std::string utf8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// The file that identifies a dump: a .gdi's first track, or the image itself.
fs::path identity(const fs::path& image) {
    if (lower_ext(image) != ".gdi")
        return image;
    std::ifstream in(image, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::vector<std::string> tracks = gdi_tracks(text);
    return tracks.empty() ? fs::path{} : image.parent_path() / tracks.front();
}

// The same file on disk as `file`, among those in a game's disc/ directory.
bool shares_file(const fs::path& disc_dir, const fs::path& file, std::uintmax_t size) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(disc_dir, ec)) {
        std::error_code e2;
        if (e.is_regular_file(e2) && e.file_size(e2) == size && fs::equivalent(e.path(), file, e2))
            return true;
    }
    return false;
}

}  // namespace

std::vector<std::string> gdi_tracks(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    std::getline(in, line);  // the number of tracks
    std::vector<std::string> files;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string number, lba, type, sector_size, name;
        if (!(fields >> number >> lba >> type >> sector_size))
            continue;
        fields >> std::ws;
        if (fields.peek() == '"') {
            fields.get();
            std::getline(fields, name, '"');
        } else {
            fields >> name;
        }
        if (name.empty() || fs::path(name).filename().string() != name)
            return {};
        files.push_back(name);
    }
    return files;
}

std::vector<Disc> find_discs(const fs::path& dir, const std::vector<Game>& games) {
    std::vector<Disc> out;
    std::error_code ec;
    auto add = [&](const fs::path& p) {
        Disc d;
        d.image = p;
        d.name = utf8(p.stem());
        const fs::path id_file = identity(p);
        std::error_code e;
        const std::uintmax_t size = id_file.empty() ? 0 : fs::file_size(id_file, e);
        if (!id_file.empty() && !e)
            for (const Game& g : games)
                if (shares_file(g.config.parent_path() / "disc", id_file, size)) {
                    d.game_id = g.id;
                    d.game_title = g.title;
                    break;
                }
        out.push_back(std::move(d));
    };
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::error_code e2;
        if (e.is_regular_file(e2) && is_image(e.path())) {
            add(e.path());
        } else if (e.is_directory(e2)) {
            for (const auto& inner : fs::directory_iterator(e.path(), e2)) {
                std::error_code e3;
                if (inner.is_regular_file(e3) && is_image(inner.path()))
                    add(inner.path());
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const Disc& a, const Disc& b) { return a.name < b.name; });
    return out;
}

}  // namespace dream::launcher
