#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <span>

#include "dream/runtime/memory.h"
#include "dream/runtime/sh4/ctx.h"
#include "dream/runtime/wince/hardware.h"
#include "dream/runtime/wince/pe_loader.h"
#include "dream/runtime/wince/threads.h"

namespace dream::wince {
class Modules;
class KernelServices;
class Synchronization;
class SystemServices;
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
    // A read-only word at a fixed address outside the process: the Dreamcast BIOS syscall
    // vectors (0x8C0000B0...) that kernel-mode code in some CE DLLs reads and jumps through.
    void map_word(std::uint32_t address, std::uint32_t value) { fixed_[address] = value; }
    // Hardware registers kernel-mode code reads and writes at addresses 0x80000000 and up.
    void set_hardware(Hardware* hardware) { hardware_ = hardware; }
    bool set_store_queue_base(std::uint32_t physical) {
        return hardware_ && hardware_->store_queue_base(physical);
    }
    // A zero-filled region at a fixed address (the KData page). False if it would overlap.
    bool map_fixed(std::uint32_t base, std::uint32_t size, unsigned permissions);
    // VirtualCopy: process addresses [base, base+size) reach the hardware at `target` onward
    // (a bus address in P2 or P4 form), through set_hardware's devices.
    void map_window(std::uint32_t base, std::uint32_t size, std::uint32_t target);
    std::size_t windows() const { return windows_.size(); }

private:
    std::map<std::uint32_t, std::uint32_t> fixed_;
    struct Window {
        std::uint32_t size, target;
    };
    std::map<std::uint32_t, Window> windows_;
    std::optional<std::uint32_t> window_target(std::uint32_t address) const;
    Hardware* hardware_ = nullptr;
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
    // The host folder CreateFileW reads from (the title's disc files).
    void set_file_root(std::filesystem::path root) { file_root_ = std::move(root); }
    // How services run guest code (window procedures, timer callbacks): fn, R4..R7 -> R0.
    using GuestCall = std::function<std::uint32_t(std::uint32_t, std::array<std::uint32_t, 4>)>;
    void set_guest_call(GuestCall call);
    // Which DLL serves a COM class (see SystemServices::ClassResolver).
    using ClassResolver =
        std::function<std::optional<std::string>(const std::array<std::uint8_t, 16>&)>;
    void set_class_resolver(ClassResolver resolver);
    std::uint32_t last_error() const;  // the calling guest thread's
    // Guest threads (CreateThread...); without a scheduler the process has one thread. With one,
    // the process also gets CE's KData page at 0x5800 (what inline GetCurrentThreadId,
    // GetCurrentProcessId and TLS reads look at), kept pointing at the running thread.
    void set_scheduler(Scheduler* scheduler, ProcessMemory* memory = nullptr);
    // Holly's interrupt status, as the OAL sees it: signals the IST events now due. `wake` as
    // Synchronization::set_event. True when any event was signalled.
    bool deliver_interrupts(std::uint32_t istnrm, std::uint32_t istext, std::uint32_t isterr,
                            bool wake);
    std::map<std::uint32_t, std::uint64_t> interrupt_counts() const;  // SYSINTR -> deliveries
    static constexpr std::uint32_t kKData = 0x5800, kProcessId = 1;
    const std::vector<std::u16string>& debug_messages() const;

private:
    std::unique_ptr<KernelServices> services_;
    std::unique_ptr<Synchronization> synchronization_;
    std::unique_ptr<SystemServices> system_;
    std::filesystem::path file_root_;
    GuestCall guest_call_;
    ClassResolver class_resolver_;
    std::map<std::uint16_t, std::string> ordinals_;
    std::map<std::uint32_t, std::uint32_t> errors_;  // last error, per guest thread
    std::map<std::uint32_t, std::uint32_t> tls_;     // allocated TLS slots
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> tls_values_;  // thread, slot
    ProcessMemory* process_ = nullptr;
    std::map<std::uint32_t, std::uint32_t> tls_blocks_;  // thread -> its 64-slot TLS array
    std::uint32_t tls_block(std::uint32_t thread);
    void point_kdata(std::uint32_t thread);
    Scheduler* scheduler_ = nullptr;
    std::uint32_t thread_id() const;
    Modules* modules_ = nullptr;
    std::uint32_t main_module_ = 0;
    std::function<bool(std::uint32_t, std::uint32_t, unsigned)> entry_call_;
};
}  // namespace dream::wince
