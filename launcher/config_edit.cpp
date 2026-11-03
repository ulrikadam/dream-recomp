#include "config_edit.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>
#include <toml.hpp>
#include <tuple>
#include <vector>

namespace dream::launcher {

namespace {

using Relocation = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;  // source, size, dest

std::set<std::uint32_t> extra_functions(const toml::table& t) {
    std::set<std::uint32_t> out;
    if (const toml::array* a = t["functions"]["extra"].as_array())
        for (const toml::node& n : *a)
            if (auto v = n.value<std::int64_t>())
                out.insert(static_cast<std::uint32_t>(*v));
    return out;
}

std::vector<Relocation> relocations(const toml::table& t) {
    std::vector<Relocation> out;
    if (const toml::array* a = t["relocations"].as_array())
        for (const toml::node& n : *a)
            if (const toml::table* r = n.as_table()) {
                const auto s = (*r)["source"].value<std::int64_t>();
                const auto z = (*r)["size"].value<std::int64_t>();
                const auto d = (*r)["dest"].value<std::int64_t>();
                if (s && z && d)
                    out.emplace_back(static_cast<std::uint32_t>(*s), static_cast<std::uint32_t>(*z),
                                     static_cast<std::uint32_t>(*d));
            }
    return out;
}

std::string hex(std::uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

std::string extra_line(const std::set<std::uint32_t>& all) {
    std::string s = "extra = [";
    bool first = true;
    for (std::uint32_t v : all) {
        if (!first)
            s += ",\n         ";
        s += hex(v);
        first = false;
    }
    return s + "]";
}

// Where a line starting with `head` begins, at the start of `text` or after a newline.
std::size_t find_line(const std::string& text, std::string_view head, std::size_t from = 0) {
    for (std::size_t at = text.find(head, from); at != std::string::npos;
         at = text.find(head, at + 1))
        if (at == 0 || text[at - 1] == '\n')
            return at;
    return std::string::npos;
}

// The next table header ("[" at the start of a line) after `from`, or the end.
std::size_t next_header(const std::string& text, std::size_t from) {
    const std::size_t at = find_line(text, "[", from);
    return at == std::string::npos ? text.size() : at;
}

void ensure_newline(std::string& s) {
    if (!s.empty() && s.back() != '\n')
        s += '\n';
}

}  // namespace

MergeResult merge_suggestions(const std::string& config, const std::string& suggested) {
    MergeResult out;
    out.text = config;
    const toml::parse_result have = toml::parse(config);
    const toml::parse_result got = toml::parse(suggested);
    if (!have || !got) {
        out.error = !have ? "config: " + std::string(have.error().description())
                          : "suggestions: " + std::string(got.error().description());
        return out;
    }

    std::set<std::uint32_t> functions = extra_functions(have.table());
    for (std::uint32_t f : extra_functions(got.table()))
        out.functions += functions.insert(f).second ? 1u : 0u;
    if (out.functions) {
        const std::string line = extra_line(functions);
        const std::size_t table = find_line(out.text, "[functions]");
        if (table != std::string::npos) {
            const std::size_t end = next_header(out.text, table + 1);
            const std::size_t key = find_line(out.text, "extra", table);
            if (key != std::string::npos && key < end) {
                const std::size_t close = out.text.find(']', key);
                out.text.replace(key, close + 1 - key, line);
            } else {
                out.text.insert(out.text.find('\n', table) + 1, line + "\n");
            }
        } else if (const std::size_t stub = out.text.find("# [functions]\n# extra = []");
                   stub != std::string::npos) {
            out.text.replace(stub, std::string("# [functions]\n# extra = []").size(),
                             "[functions]\n" + line);
        } else {
            ensure_newline(out.text);
            out.text += "\n[functions]\n" + line + "\n";
        }
    }

    std::vector<Relocation> known = relocations(have.table());
    std::string added;
    for (const Relocation& r : relocations(got.table())) {
        if (std::find(known.begin(), known.end(), r) != known.end())
            continue;
        known.push_back(r);
        ++out.relocations;
        added += "\n[[relocations]]\nsource = " + hex(std::get<0>(r)) +
                 "\nsize = " + hex(std::get<1>(r)) + "\ndest = " + hex(std::get<2>(r)) + "\n";
    }
    if (!added.empty()) {
        ensure_newline(out.text);
        out.text += "\n# Code the program copied and ran elsewhere, found by the launcher." + added;
    }
    return out;
}

std::string set_disc_image(const std::string& config, std::string_view image) {
    std::string text = config;
    const std::size_t table = find_line(text, "[disc]");
    if (table == std::string::npos)
        return text;
    const std::size_t end = next_header(text, table + 1);
    const std::size_t key = find_line(text, "image", table);
    if (key == std::string::npos || key >= end)
        return text;
    std::size_t eol = text.find('\n', key);
    if (eol == std::string::npos)
        eol = text.size();
    std::string escaped;
    for (char c : image) {
        if (c == '"' || c == '\\')
            escaped += '\\';
        escaped += c;
    }
    text.replace(key, eol - key, "image = \"" + escaped + "\"");
    return text;
}

std::string set_status(const std::string& config, std::string_view level, std::string_view note) {
    static constexpr std::string_view kComment =
        "# How far the game gets, for the launcher: playable, ingame, boots, nothing or "
        "unsupported.\n";
    std::string text = config;
    const std::size_t table = find_line(text, "[status]");
    if (table != std::string::npos) {
        std::size_t begin = table;
        if (begin >= kComment.size() &&
            text.compare(begin - kComment.size(), kComment.size(), kComment) == 0)
            begin -= kComment.size();
        text.erase(begin, next_header(text, table + 1) - begin);
        while (text.size() >= 2 && text[text.size() - 1] == '\n' && text[text.size() - 2] == '\n')
            text.pop_back();
    }
    std::string escaped;
    for (char c : note) {
        if (c == '"' || c == '\\')
            escaped += '\\';
        escaped += c;
    }
    ensure_newline(text);
    text += "\n";
    text += kComment;
    text += "[status]\nlevel = \"" + std::string(level) + "\"\nnote = \"" + escaped + "\"\n";
    return text;
}

}  // namespace dream::launcher
