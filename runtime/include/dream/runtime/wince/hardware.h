#pragma once
#include <cstdint>

namespace dream::wince {
// Kernel-mode accesses a CE process makes outside its own memory (SetKMode, then a hardware
// address). ProcessMemory asks this before refusing them. Each method returns false for an
// address it does not emulate.
class Hardware {
public:
    virtual ~Hardware() = default;
    virtual bool read(std::uint32_t address, unsigned size, std::uint64_t& value) = 0;
    virtual bool write(std::uint32_t address, unsigned size, std::uint64_t value) = 0;
    // Store queues (0xE0000000-0xE3FFFFFF and PREF): how drivers feed the TA FIFO.
    virtual bool store_queue_write(std::uint32_t, std::uint32_t) { return false; }
    virtual bool store_queue_flush(std::uint32_t) { return false; }
    // SetStoreQueueBase: from now on the whole store-queue area maps onto `physical` onward, in
    // 1 MiB pages, as the kernel's TLB miss handler maps it.
    virtual bool store_queue_base(std::uint32_t) { return false; }
};

// The few Dreamcast registers titles touch directly. So far: the SH-4 port A pair (PCTRA,
// PDTRA) through which software reads the AV cable type.
class DreamcastHardware final : public Hardware {
public:
    enum class Cable : std::uint32_t { Vga = 0, Rgb = 2, Composite = 3 };
    explicit DreamcastHardware(Cable cable = Cable::Vga) : cable_(cable) {}
    bool read(std::uint32_t address, unsigned size, std::uint64_t& value) override;
    bool write(std::uint32_t address, unsigned size, std::uint64_t value) override;

private:
    Cable cable_;
    std::uint32_t pctra_ = 0, pdtra_ = 0;
};
}  // namespace dream::wince
