#pragma once

#include <array>
#include <map>
#include <memory>
#include <span>

#include "dream/runtime/memory.h"
#include "dream/runtime/sh4/ctx.h"
#include "dream/runtime/wince/pe_loader.h"

namespace dream::wince {
class Modules;
class KernelServices;
class Synchronization;
struct MemoryError : std::runtime_error {
    std::uint32_t code;
    MemoryError(std::uint32_t error, const char* message)
        : std::runtime_error(message), code(error) {}
};
// Software address space for HLE applications. It replaces OS page management; MmuMemory is
// the separate hardware path for experiments with privileged SH-4/kernel code.
class ProcessMemory final : public Memory {
public:
    enum Permission : unsigned { Read = 1, Write = 2, Execute = 4 };
    static constexpr std::uint32_t kBudget = 64u << 20;
    void install(const Image& image);
    std::uint32_t allocate(std::uint32_t bytes);
    bool release(std::uint32_t base);
    // HLE Virtual* area, with 4 KiB pages and 64 KiB reservation alignment.
    std::uint32_t virtual_alloc(std::uint32_t address, std::uint32_t size, std::uint32_t type,
                                std::uint32_t protection);
    void virtual_free(std::uint32_t address, std::uint32_t size, std::uint32_t type);
    void virtual_protect(std::uint32_t address, std::uint32_t size, std::uint32_t protection,
                         std::uint32_t old_protection_pointer);
    std::array<std::uint32_t, 7> virtual_query(std::uint32_t address) const;
    void check(std::uint32_t address, std::uint32_t size, unsigned permission) const;
    std::vector<std::uint8_t> read_bytes(std::uint32_t address, std::uint32_t size) const;
    void write_bytes(std::uint32_t address, std::span<const std::uint8_t> bytes);
    std::uint8_t read8(std::uint32_t a) override;
    std::uint16_t read16(std::uint32_t a) override;
    std::uint16_t fetch16(std::uint32_t a) override;
    std::uint32_t read32(std::uint32_t a) override;
    std::uint64_t read64(std::uint32_t a) override;
    void write8(std::uint32_t a, std::uint8_t v) override;
    void write16(std::uint32_t a, std::uint16_t v) override;
    void write32(std::uint32_t a, std::uint32_t v) override;
    void write64(std::uint32_t a, std::uint64_t v) override;
    void sq_write32(std::uint32_t, std::uint32_t) override;
    void sq_flush(std::uint32_t) override;

private:
    struct Region {
        std::vector<std::uint8_t> bytes;
        unsigned permissions;
        bool allocation;
        std::uint32_t allocation_base = 0;
    };
    std::map<std::uint32_t, Region> regions_;
    std::uint32_t used_ = 0;
    std::uint32_t allocation_count_ = 0;
    struct Reservation {
        std::uint32_t size, protection;
        std::vector<std::uint32_t> pages;  // 0 = reserved; otherwise PAGE_* of a committed page.
    };
    std::map<std::uint32_t, Reservation> reservations_;
    std::uint64_t load(std::uint32_t a, unsigned size, unsigned permission = Read) const;
    void store(std::uint32_t a, std::uint64_t v, unsigned size);
};

struct ProcessExit {
    std::uint32_t code;
};
// Single-threaded first set of COREDLL services. No invented ordinal table: aliases must be
// supplied from a verified module inventory. Unsupported imports remain unresolved.
class Kernel {
public:
    Kernel();
    ~Kernel();
    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;
    static constexpr std::uint32_t kTrapBase = 0x1F000000u;
    std::optional<std::uint32_t> resolve(const Import& import) const;
    void bind_ordinal(std::uint16_t ordinal, const std::string& supported_name);
    void attach_modules(Modules& modules, std::uint32_t main_module,
                        std::function<bool(std::uint32_t, std::uint32_t, unsigned)> entry_call);
    bool invoke(sh4::Ctx& cpu, Memory& memory, std::uint32_t target);
    std::uint32_t last_error() const { return last_error_; }
    const std::vector<std::u16string>& debug_messages() const;

private:
    std::unique_ptr<KernelServices> services_;
    std::unique_ptr<Synchronization> synchronization_;
    std::map<std::uint16_t, std::string> ordinals_;
    std::uint32_t last_error_ = 0;
    std::map<std::uint32_t, std::uint32_t> tls_;
    Modules* modules_ = nullptr;
    std::uint32_t main_module_ = 0;
    std::function<bool(std::uint32_t, std::uint32_t, unsigned)> entry_call_;
};
}  // namespace dream::wince
