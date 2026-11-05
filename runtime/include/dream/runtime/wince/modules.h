#pragma once
#include <map>

#include "dream/runtime/wince/process.h"

namespace dream::wince {
// File providers receive a normalized basename only. They must keep reads inside their module
// directory; no host DLL is ever loaded as native code.
using ModuleProvider = std::function<std::vector<std::uint8_t>(const std::string&)>;
class Modules {
public:
    struct Module {
        std::string name;
        Image image;
        bool initialized = false;
    };
    using EntryCall =
        std::function<bool(std::uint32_t entry, std::uint32_t instance, unsigned reason)>;
    Modules(ProcessMemory& memory, ModuleProvider provider, ImportResolver hle)
        : memory_(memory), provider_(std::move(provider)), hle_(std::move(hle)) {}
    std::uint32_t load(const std::string& name, std::span<const std::uint8_t> file,
                       std::optional<std::uint32_t> base = {});
    std::uint32_t load_library(const std::string& name);
    void initialize(const EntryCall& call);
    std::uint32_t handle(const std::string& name) const;
    std::uint32_t procedure(std::uint32_t module, const Import& symbol) const;
    const std::map<std::string, Module>& all() const { return modules_; }
    static std::string normalize(std::string name);
    void register_hle(std::string name, std::uint32_t handle);

private:
    ProcessMemory& memory_;
    ModuleProvider provider_;
    ImportResolver hle_;
    std::map<std::string, Module> modules_;
    std::map<std::string, std::uint32_t> hle_modules_;
    std::vector<std::string> pending_;
    bool initializing_ = false, failed_ = false;
    std::uint32_t resolve(const Import& symbol, const std::map<std::string, Module>& staged,
                          unsigned depth = 0) const;
};
}  // namespace dream::wince
