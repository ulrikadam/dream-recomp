#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace dream::wince {
struct PeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Import {
    std::string dll, name;
    std::optional<std::uint16_t> ordinal;
    std::uint32_t iat_rva = 0, address = 0;
};
struct Section {
    std::string name;
    std::uint32_t rva = 0, size = 0, characteristics = 0;
};
struct Export {
    std::string name, forwarder;
    std::uint32_t ordinal = 0, rva = 0;
};
struct Image {
    std::uint32_t base = 0, entry = 0, headers_size = 0;
    bool dll = false;
    std::vector<std::uint8_t> bytes;
    std::vector<Section> sections;
    std::vector<Import> imports;
    std::vector<Export> exports;
};
using ImportResolver = std::function<std::optional<std::uint32_t>(const Import&)>;

// Two-phase loading permits a dependency graph (including cycles) to be prepared before IATs
// are bound. An unbound image must not be installed or executed by a process.
Image prepare_pe(std::span<const std::uint8_t> file, std::optional<std::uint32_t> base = {});
void bind_imports(Image& image, const ImportResolver& resolver);

// Ordinary PE32 / SH-4 / CE images only, at most 32 MiB. Produces an isolated image;
// no guest memory is modified before all validation, relocations and imports have succeeded.
// The resolver supplies guest addresses, never host pointers. Unresolved imports are errors.
Image load_pe(std::span<const std::uint8_t> file, std::optional<std::uint32_t> base = {},
              const ImportResolver& resolver = {});
}  // namespace dream::wince
