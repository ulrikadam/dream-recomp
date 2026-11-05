#include "dream/runtime/wince/modules.h"

#include <algorithm>
#include <charconv>

namespace dream::wince {
std::string Modules::normalize(std::string name) {
    if (name.empty() || name.size() > 260 || name == "." || name.find("..") != std::string::npos)
        throw PeError("invalid module basename");
    for (auto& c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-'))
            throw PeError("invalid module basename");
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + 32);
    }
    if (name.find('.') == std::string::npos)
        name += ".dll";
    return name;
}
void Modules::register_hle(std::string name, std::uint32_t h) {
    name = normalize(std::move(name));
    if (!h || modules_.contains(name) || hle_modules_.contains(name))
        throw PeError("duplicate HLE module");
    for (const auto& [n, old] : hle_modules_) {
        (void)n;
        if (old == h)
            throw PeError("duplicate HLE handle");
    }
    hle_modules_.emplace(name, h);
}
std::uint32_t Modules::handle(const std::string& name) const {
    const auto key = normalize(name);
    if (const auto it = hle_modules_.find(key); it != hle_modules_.end())
        return it->second;
    const auto it = modules_.find(key);
    return it == modules_.end() ? 0 : it->second.image.base;
}
std::uint32_t Modules::resolve(const Import& symbol, const std::map<std::string, Module>& staged,
                               unsigned depth) const {
    if (depth >= 32)
        throw PeError("export forwarding cycle or excessive depth");
    if (hle_)
        if (auto address = hle_(symbol))
            return *address;
    const auto name = normalize(symbol.dll);
    const Image* image = nullptr;
    if (const auto it = staged.find(name); it != staged.end())
        image = &it->second.image;
    else if (const auto old = modules_.find(name); old != modules_.end())
        image = &old->second.image;
    if (!image)
        return 0;
    for (const auto& e : image->exports) {
        if (symbol.ordinal ? e.ordinal != *symbol.ordinal : e.name != symbol.name)
            continue;
        if (e.forwarder.empty())
            return image->base + e.rva;
        const auto dot = e.forwarder.rfind('.');
        if (dot == std::string::npos || dot + 1 == e.forwarder.size())
            throw PeError("invalid export forwarder");
        Import forwarded;
        forwarded.dll = normalize(e.forwarder.substr(0, dot));
        forwarded.name = e.forwarder.substr(dot + 1);
        if (forwarded.name.front() == '#') {
            unsigned ordinal = 0;
            const auto text = std::string_view(forwarded.name).substr(1);
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), ordinal);
            if (ec != std::errc{} || end != text.data() + text.size() || ordinal > 65535)
                throw PeError("invalid forwarded ordinal");
            forwarded.ordinal = static_cast<std::uint16_t>(ordinal);
        }
        return resolve(forwarded, staged, depth + 1);
    }
    return 0;
}
std::uint32_t Modules::procedure(std::uint32_t module, const Import& symbol) const {
    if (failed_)
        throw PeError("module initialization previously failed");
    auto request = symbol;
    for (const auto& [name, h] : hle_modules_)
        if (h == module) {
            request.dll = name;
            return resolve(request, {});
        }
    for (const auto& [name, m] : modules_)
        if (m.image.base == module) {
            request.dll = name;
            return resolve(request, {});
        }
    return 0;
}
std::uint32_t Modules::load(const std::string& requested, std::span<const std::uint8_t> file,
                            std::optional<std::uint32_t> base) {
    if (initializing_ || failed_)
        throw PeError("module loading unavailable during or after failed initialization");
    const auto root = normalize(requested);
    if (const auto old = handle(root))
        return old;
    std::map<std::string, Module> staged;
    std::vector<std::string> order;
    std::size_t total = 0;
    auto available = [&](std::uint32_t at, std::size_t size) {
        if (at < 0x10000 || static_cast<std::uint64_t>(at) + size > 0x10000000ull)
            return false;
        for (const auto* table : {&modules_, &staged})
            for (const auto& [name, m] : *table) {
                (void)name;
                if (at < m.image.base + m.image.bytes.size() && m.image.base < at + size)
                    return false;
            }
        return true;
    };
    std::function<void(const std::string&, std::span<const std::uint8_t>,
                       std::optional<std::uint32_t>, unsigned)>
        visit;
    visit = [&](const std::string& name, std::span<const std::uint8_t> bytes,
                std::optional<std::uint32_t> address, unsigned depth) {
        if (modules_.contains(name) || staged.contains(name) || hle_modules_.contains(name))
            return;
        if (depth > 64 || staged.size() + modules_.size() >= 128)
            throw PeError("module dependency limit exceeded");
        auto image = prepare_pe(bytes, address);
        if (name != root && !image.dll)
            throw PeError("dependency is not a DLL: " + name);
        if (!available(image.base, image.bytes.size())) {
            if (address)
                throw PeError("requested module address overlaps or is outside user image space");
            std::uint32_t chosen = 0x01000000;
            while (chosen < 0x10000000 && !available(chosen, image.bytes.size())) chosen += 65536;
            if (!available(chosen, image.bytes.size()))
                throw PeError("no free module address range");
            image = prepare_pe(bytes, chosen);
        }
        total += image.bytes.size();
        if (total > ProcessMemory::kBudget)
            throw PeError("module memory budget exceeded");
        staged.emplace(name, Module{name, std::move(image), false});
        auto dependency = [&](std::string dep) {
            dep = normalize(std::move(dep));
            if (modules_.contains(dep) || staged.contains(dep) || hle_modules_.contains(dep))
                return;
            if (!provider_)
                throw PeError("no module provider for " + dep);
            const auto data = provider_(dep);
            visit(dep, data, {}, depth + 1);
        };
        const auto& im = staged.at(name).image;
        for (const auto& i : im.imports)
            if (!hle_ || !hle_(i))
                dependency(i.dll);
        for (const auto& e : im.exports)
            if (!e.forwarder.empty()) {
                const auto dot = e.forwarder.rfind('.');
                if (dot == std::string::npos)
                    throw PeError("invalid export forwarder");
                dependency(e.forwarder.substr(0, dot));
            }
        order.push_back(name);
    };
    visit(root, file, base, 0);
    for (auto& [name, m] : staged) {
        try {
            bind_imports(m.image, [&](const Import& i) -> std::optional<std::uint32_t> {
                return resolve(i, staged);
            });
        } catch (const PeError& e) {
            throw PeError(name + ": " + e.what());
        }
    }
    // All-or-nothing installation, including conflicts with existing process allocations.
    auto candidate = memory_;
    for (const auto& name : order) candidate.install(staged.at(name).image);
    auto next_pending = pending_;
    next_pending.insert(next_pending.end(), order.begin(), order.end());
    const auto result = staged.at(root).image.base;
    memory_ = std::move(candidate);
    modules_.merge(staged);
    pending_ = std::move(next_pending);
    return result;
}
std::uint32_t Modules::load_library(const std::string& name) {
    if (failed_ || initializing_)
        throw PeError("recursive or failed module initialization");
    const auto key = normalize(name);
    if (const auto old = handle(key))
        return old;
    if (!provider_)
        throw PeError("no module provider for " + key);
    const auto data = provider_(key);
    if (!prepare_pe(data).dll)
        throw PeError("LoadLibrary requires a DLL");
    return load(key, data);
}
void Modules::initialize(const EntryCall& call) {
    if (initializing_ || failed_)
        throw PeError("recursive or failed module initialization");
    initializing_ = true;
    try {
        for (const auto& name : pending_) {
            auto& m = modules_.at(name);
            if (!m.initialized && m.image.dll && m.image.entry &&
                (!call || !call(m.image.entry, m.image.base, 1)))
                throw PeError("DLL_PROCESS_ATTACH failed: " + name);
            m.initialized = true;
        }
        pending_.clear();
        initializing_ = false;
    } catch (...) {
        initializing_ = false;
        failed_ = true;
        throw;
    }
}
}  // namespace dream::wince
