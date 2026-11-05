#include "dream/runtime/wince/pe_loader.h"

#include <algorithm>
#include <array>
#include <utility>

namespace dream::wince {
namespace {
constexpr std::uint32_t kMaxImage = 32u * 1024 * 1024;
void require(bool ok, const char* message) {
    if (!ok)
        throw PeError(message);
}
bool fits(std::size_t at, std::size_t size, std::size_t limit) {
    return at <= limit && size <= limit - at;
}
std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t p) {
    require(fits(p, 2, b.size()), "truncated PE word");
    return static_cast<std::uint16_t>(b[p] | (static_cast<unsigned>(b[p + 1]) << 8));
}
std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t p) {
    return u16(b, p) | (static_cast<std::uint32_t>(u16(b, p + 2)) << 16);
}
void put16(std::vector<std::uint8_t>& b, std::size_t p, std::uint16_t v) {
    require(fits(p, 2, b.size()), "relocation outside image");
    b[p] = static_cast<std::uint8_t>(v);
    b[p + 1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::vector<std::uint8_t>& b, std::size_t p, std::uint32_t v) {
    put16(b, p, static_cast<std::uint16_t>(v));
    put16(b, p + 2, static_cast<std::uint16_t>(v >> 16));
}
bool mapped(const Image& im, std::uint32_t at, std::uint32_t size) {
    if (!fits(at, size, im.bytes.size()))
        return false;
    if (fits(at, size, im.headers_size))
        return true;
    for (const auto& s : im.sections)
        if (at >= s.rva && fits(at - s.rva, size, s.size))
            return true;
    return false;
}
std::string string_at(const Image& im, std::uint32_t at) {
    std::string out;
    for (std::uint32_t i = 0; i <= 260; ++i) {
        require(mapped(im, at + i, 1), "unmapped import string");
        const auto c = im.bytes[at + i];
        if (!c) {
            require(!out.empty(), "empty import name");
            return out;
        }
        require(c >= 32 && c < 127, "non-ASCII import name");
        out.push_back(static_cast<char>(c));
    }
    throw PeError("unterminated import name");
}
bool power_two(std::uint32_t x) {
    return x && !(x & (x - 1));
}
}  // namespace

Image prepare_pe(std::span<const std::uint8_t> file, std::optional<std::uint32_t> requested) {
    require(file.size() <= kMaxImage && file.size() >= 64, "PE file size outside supported bounds");
    require(u16(file, 0) == 0x5A4D, "missing DOS MZ signature");
    const std::size_t pe = u32(file, 0x3C);
    require(pe >= 64 && fits(pe, 24, file.size()), "invalid PE header offset");
    require(u32(file, pe) == 0x4550, "missing PE signature");
    require(u16(file, pe + 4) == 0x1A6, "expected SH-4 machine (0x01A6)");
    const auto count = u16(file, pe + 6), opt_size = u16(file, pe + 20);
    const auto flags = u16(file, pe + 22);
    require((flags & 2) != 0, "PE is not an executable image");
    require(count > 0 && count <= 96, "unsupported section count");
    const auto opt = pe + 24, table = opt + opt_size;
    require(opt_size >= 96 && fits(opt, opt_size, file.size()), "truncated optional header");
    require(u16(file, opt) == 0x10B, "expected PE32 optional header");
    require(u16(file, opt + 68) == 9, "expected Windows CE subsystem");
    const auto preferred = u32(file, opt + 28), entry_rva = u32(file, opt + 16);
    const auto section_align = u32(file, opt + 32), file_align = u32(file, opt + 36);
    require(power_two(section_align) && power_two(file_align), "invalid PE alignment");
    require(section_align >= file_align &&
                (section_align >= 4096 ? file_align >= 512 && file_align <= 65536
                                       : file_align == section_align),
            "unsupported PE alignment");
    const auto size = u32(file, opt + 56), headers = u32(file, opt + 60);
    require(size > 0 && size <= kMaxImage && size % section_align == 0, "invalid SizeOfImage");
    require(headers > 0 && headers <= size && headers <= file.size() && headers % file_align == 0,
            "invalid SizeOfHeaders");
    require(fits(table, static_cast<std::size_t>(count) * 40, headers),
            "section table outside headers");
    Image im;
    im.base = requested.value_or(preferred);
    require(im.base % 65536u == 0 && static_cast<std::uint64_t>(im.base) + size <= 0x100000000ull,
            "invalid image base");
    im.dll = (flags & 0x2000) != 0;
    im.headers_size = headers;
    im.bytes.resize(size);  // BSS and section tails are zero-filled
    std::copy_n(file.begin(), headers, im.bytes.begin());
    std::vector<std::pair<std::uint32_t, std::uint32_t>> raw_ranges;
    for (unsigned i = 0; i < count; ++i) {
        const auto p = table + i * 40;
        Section s;
        for (std::size_t n = 0; n < 8 && file[p + n]; ++n) s.name += static_cast<char>(file[p + n]);
        const auto raw_size = u32(file, p + 16), raw = u32(file, p + 20);
        s.rva = u32(file, p + 12);
        s.size = std::max(u32(file, p + 8), raw_size);
        s.characteristics = u32(file, p + 36);
        require(s.rva >= headers && s.rva % section_align == 0 && fits(s.rva, s.size, size),
                "section outside image or misaligned");
        for (const auto& prev : im.sections)
            require(!s.size || !prev.size || s.rva >= prev.rva + prev.size ||
                        prev.rva >= s.rva + s.size,
                    "overlapping virtual sections");
        if (raw_size) {
            require(raw >= headers && raw % file_align == 0 && raw_size % file_align == 0 &&
                        fits(raw, raw_size, file.size()),
                    "section raw data outside file or misaligned");
            if (section_align < 4096)
                require(raw == s.rva, "low-alignment section raw offset differs from RVA");
            for (const auto& range : raw_ranges)
                require(raw >= range.second || range.first >= raw + raw_size,
                        "overlapping raw sections");
            raw_ranges.emplace_back(raw, raw + raw_size);
            std::copy_n(file.begin() + raw, raw_size, im.bytes.begin() + s.rva);
        }
        im.sections.push_back(std::move(s));
    }
    if (entry_rva) {
        require((entry_rva & 1u) == 0, "unaligned SH-4 entry point");
        bool executable = false;
        for (const auto& s : im.sections)
            if (entry_rva >= s.rva && fits(entry_rva - s.rva, 2, s.size) &&
                (s.characteristics & 0x20000000u))
                executable = true;
        require(executable, "entry point is not in an executable section");
        im.entry = im.base + entry_rva;
    } else {
        require(im.dll, "EXE has no entry point");
    }
    const auto dirs = u32(file, opt + 92);
    require(dirs <= (opt_size - 96u) / 8u, "data directories exceed optional header");
    std::array<std::pair<std::uint32_t, std::uint32_t>, 16> directory{};
    for (unsigned i = 0; i < std::min(dirs, 16u); ++i) {
        const auto rva = u32(file, opt + 96 + i * 8), len = u32(file, opt + 100 + i * 8);
        require((rva == 0) == (len == 0), "incomplete data directory");
        if (len)
            require(i == 4 ? fits(rva, len, file.size()) : mapped(im, rva, len),
                    "data directory outside mapped image");
        directory[i] = {rva, len};
    }
    require(!directory[9].second, "TLS initialization is not implemented");
    require(!directory[13].second, "delay imports are not implemented");
    require(!directory[14].second, "managed PE images are not supported");

    const auto delta = im.base - preferred;
    const auto [reloc, reloc_size] = directory[5];
    if (delta)
        require(!(flags & 1) && reloc_size, "image cannot be rebased without relocations");
    // Validate even at the preferred address. Stage patches so relocation targets cannot mutate
    // the relocation table while it is being parsed.
    struct Patch {
        std::uint32_t at, value;
        bool word;
    };
    std::vector<Patch> patches;
    for (std::uint32_t off = 0; off < reloc_size;) {
        require(reloc_size - off >= 8, "truncated relocation block");
        const auto page = u32(im.bytes, reloc + off), block = u32(im.bytes, reloc + off + 4);
        require((page & 4095u) == 0 && block >= 8 && block % 4 == 0 && block <= reloc_size - off,
                "invalid relocation block");
        for (std::uint32_t n = 8; n < block; n += 2) {
            const auto item = u16(im.bytes, reloc + off + n);
            const auto type = item >> 12;
            if (!type)
                continue;
            require(page <= size && (item & 4095u) <= size - page, "relocation target overflow");
            const auto at = page + (item & 4095u);
            require(mapped(im, at, type == 3 ? 4u : 2u), "unmapped relocation target");
            std::uint32_t value = 0;
            if (type == 3)
                value = u32(im.bytes, at) + delta;
            else if (type == 1)
                value = u16(im.bytes, at) + (delta >> 16);
            else if (type == 2)
                value = u16(im.bytes, at) + (delta & 65535u);
            else if (type == 4) {
                n += 2;
                require(n < block, "HIGHADJ relocation missing its pair");
                value = (static_cast<std::uint32_t>(u16(im.bytes, at)) << 16) +
                        static_cast<std::uint32_t>(
                            static_cast<std::int16_t>(u16(im.bytes, reloc + off + n)));
                value = (value + delta + 0x8000u) >> 16;
            } else
                throw PeError("unsupported SH-4 base relocation type " + std::to_string(type));
            if (delta)
                patches.push_back({at, value, type != 3});
        }
        off += block;
    }
    for (const auto& p : patches) {
        if (p.word)
            put16(im.bytes, p.at, static_cast<std::uint16_t>(p.value));
        else
            put32(im.bytes, p.at, p.value);
    }

    const auto [imports, imports_size] = directory[1];
    if (imports_size) {
        bool terminated = false;
        for (std::uint32_t off = 0; fits(off, 20, imports_size); off += 20) {
            require(off / 20 < 512, "too many import descriptors");
            const auto p = imports + off;
            const auto original = u32(im.bytes, p), name = u32(im.bytes, p + 12),
                       iat = u32(im.bytes, p + 16);
            if (!(original | name | iat | u32(im.bytes, p + 4) | u32(im.bytes, p + 8))) {
                terminated = true;
                break;
            }
            require(name && iat && (iat & 3u) == 0 && (original & 3u) == 0,
                    "invalid import descriptor");
            const auto dll = string_at(im, name);
            const auto lookup = original ? original : iat;
            require(lookup < size && iat < size, "import thunk outside image");
            for (std::uint32_t n = 0;; ++n) {
                require(n < 65536 && im.imports.size() < 65536,
                        "unterminated or oversized import table");
                require(mapped(im, lookup + n * 4, 4) && mapped(im, iat + n * 4, 4),
                        "unmapped import thunk");
                const auto thunk = u32(im.bytes, lookup + n * 4);
                if (!thunk)
                    break;
                Import imp;
                imp.dll = dll;
                imp.iat_rva = iat + n * 4;
                if (thunk & 0x80000000u) {
                    require(!(thunk & 0x7FFF0000u), "invalid import ordinal encoding");
                    imp.ordinal = static_cast<std::uint16_t>(thunk);
                } else {
                    require(mapped(im, thunk, 3), "unmapped import hint/name");
                    imp.name = string_at(im, thunk + 2);
                }
                im.imports.push_back(std::move(imp));
            }
        }
        require(terminated, "unterminated import descriptor table");
    }
    const auto [exports, exports_size] = directory[0];
    if (exports_size) {
        require(exports_size >= 40, "truncated export directory");
        const auto first = u32(im.bytes, exports + 16),
                   count_functions = u32(im.bytes, exports + 20);
        const auto count_names = u32(im.bytes, exports + 24),
                   functions = u32(im.bytes, exports + 28);
        const auto names = u32(im.bytes, exports + 32), ordinals = u32(im.bytes, exports + 36);
        require(count_functions <= 65536 && count_names <= 65536 && first <= 65535 &&
                    count_functions <= 65536 - first,
                "excessive export table");
        require(mapped(im, functions, count_functions * 4) && mapped(im, names, count_names * 4) &&
                    mapped(im, ordinals, count_names * 2),
                "unmapped export table");
        std::vector<std::size_t> function_indices(count_functions, static_cast<std::size_t>(-1));
        for (std::uint32_t i = 0; i < count_functions; ++i) {
            const auto rva = u32(im.bytes, functions + i * 4);
            if (!rva)
                continue;  // unused ordinal slot
            Export e;
            e.ordinal = first + i;
            e.rva = rva;
            if (rva >= exports && rva - exports < exports_size) {
                e.forwarder = string_at(im, rva);
                require(e.forwarder.size() + 1 <= exports_size - (rva - exports),
                        "forwarder string outside export directory");
            } else
                require(mapped(im, rva, 1), "unmapped exported address");
            function_indices[i] = im.exports.size();
            im.exports.push_back(std::move(e));
        }
        std::vector<std::string> seen;
        for (std::uint32_t i = 0; i < count_names; ++i) {
            const auto index = u16(im.bytes, ordinals + i * 2);
            require(index < count_functions, "export ordinal index out of range");
            const auto name = string_at(im, u32(im.bytes, names + i * 4));
            seen.push_back(name);
            const auto position = function_indices[index];
            require(position != static_cast<std::size_t>(-1),
                    "named export points to an empty ordinal slot");
            auto& e = im.exports[position];
            if (e.name.empty())
                e.name = name;
            else {
                Export alias = e;
                alias.name = name;
                im.exports.push_back(std::move(alias));
            }
        }
        std::sort(seen.begin(), seen.end());
        require(std::adjacent_find(seen.begin(), seen.end()) == seen.end(),
                "duplicate export name");
    }
    return im;
}

void bind_imports(Image& im, const ImportResolver& resolver) {
    std::vector<std::uint32_t> addresses;
    for (const auto& imp : im.imports) {
        const auto address = resolver ? resolver(imp) : std::nullopt;
        if (!address || !*address)
            throw PeError("unresolved import: " + imp.dll + "!" +
                          (imp.ordinal ? "#" + std::to_string(*imp.ordinal) : imp.name));
        addresses.push_back(*address);  // data exports need not be instruction-aligned
    }
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        auto& imp = im.imports[i];
        require(mapped(im, imp.iat_rva, 4), "invalid IAT destination");
    }
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        im.imports[i].address = addresses[i];
        put32(im.bytes, im.imports[i].iat_rva, addresses[i]);
    }
}

Image load_pe(std::span<const std::uint8_t> file, std::optional<std::uint32_t> requested,
              const ImportResolver& resolver) {
    auto im = prepare_pe(file, requested);
    bind_imports(im, resolver);
    return im;
}
}  // namespace dream::wince
