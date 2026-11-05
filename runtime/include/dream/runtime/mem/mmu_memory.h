#pragma once

#include <stdexcept>

#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/sh4/ctx.h"

namespace dream::mem {

// A synchronous CPU fault, never a successful zero read. The experimental caller stops on it;
// delivery to a real CE kernel (SSR/SPC, bank switching, delay-slot restart) is not implemented.
struct MmuFault : std::runtime_error {
    std::uint32_t address, event, vector;
    bool reset;  // multiple-hit is a manual reset; vector is then the absolute reset address
    MmuFault(std::uint32_t address, std::uint32_t event);
};

// Opt-in CPU bus. DcMemory remains the Katana/physical-device bus. DMA must use DcMemory.
// Instruction translation searches the UTLB directly; ITLB caching/replacement is not simulated.
class MmuMemory final : public Memory {
public:
    MmuMemory(DcMemory& bus, sh4::Ctx& cpu) : bus_(bus), cpu_(cpu) {}
    std::uint8_t read8(std::uint32_t a) override;
    std::uint16_t read16(std::uint32_t a) override;
    std::uint16_t fetch16(std::uint32_t a) override;
    std::uint32_t read32(std::uint32_t a) override;
    std::uint64_t read64(std::uint32_t a) override;
    void write8(std::uint32_t a, std::uint8_t v) override;
    void write16(std::uint32_t a, std::uint16_t v) override;
    void write32(std::uint32_t a, std::uint32_t v) override;
    void write64(std::uint32_t a, std::uint64_t v) override;
    void sq_write32(std::uint32_t a, std::uint32_t v) override;
    void sq_flush(std::uint32_t a) override;
    void ldtlb() override;

private:
    std::uint32_t translate(std::uint32_t a, unsigned size, bool write, bool fetch = false);
    std::uint32_t page(std::uint32_t a, bool write);
    [[noreturn]] void fault(std::uint32_t a, std::uint32_t event);
    DcMemory& bus_;
    sh4::Ctx& cpu_;
};
}  // namespace dream::mem
