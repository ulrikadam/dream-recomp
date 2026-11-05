#include "dream/runtime/mem/mmu_memory.h"

#include <cstdio>

namespace dream::mem {
namespace {
std::string description(std::uint32_t a, std::uint32_t event) {
    char text[96];
    std::snprintf(text, sizeof(text), "SH-4 MMU fault: VA 0x%08x, EXPEVT 0x%03x", a, event);
    return text;
}
}  // namespace
MmuFault::MmuFault(std::uint32_t a, std::uint32_t e)
    : std::runtime_error(description(a, e)),
      address(a),
      event(e),
      vector(e == 0x140 ? 0xA0000000u : (e == 0x40 || e == 0x60 ? 0x400u : 0x100u)),
      reset(e == 0x140) {}

void MmuMemory::fault(std::uint32_t a, std::uint32_t event) {
    bus_.write32(0xFF00000Cu, a);  // TEA
    if (event != 0x140) {
        const auto asid = bus_.read32(0xFF000000u) & 255u;
        bus_.write32(0xFF000000u, (a & 0xFFFFFC00u) | asid);
    }
    bus_.expevt = event;
    throw MmuFault(a, event);
}

std::uint32_t MmuMemory::page(std::uint32_t a, bool write) {
    std::uint32_t pa = 0;
    const auto control = bus_.read32(0xFF000010u);
    const auto boundary = (control >> 18) & 63u;
    auto replacement = ((control >> 10) + 1u) & 63u;
    if (boundary && replacement == boundary)
        replacement = 0;
    bus_.write32(0xFF000010u, (control & ~0xFC00u) | (replacement << 10));
    const auto result =
        bus_.utlb().lookup(a, static_cast<std::uint8_t>(bus_.read32(0xFF000000u)),
                           (cpu_.sr & sh4::SR_MD) != 0, (control & 0x100u) != 0, write, pa);
    switch (result) {
        case Utlb::Result::Hit:
            return pa;
        case Utlb::Result::Miss:
            fault(a, write ? 0x60u : 0x40u);
        case Utlb::Result::MultipleHit:
            fault(a, 0x140u);
        case Utlb::Result::Protection:
            fault(a, write ? 0xC0u : 0xA0u);
        case Utlb::Result::InitialWrite:
            fault(a, 0x80u);
    }
    fault(a, 0x140u);
}

std::uint32_t MmuMemory::translate(std::uint32_t a, unsigned size, bool write, bool fetch) {
    const bool privileged = (cpu_.sr & sh4::SR_MD) != 0;
    // SH-4 double transfers require word alignment; preflight both words in read/write64.
    if (a & (size - 1u))
        fault(a, write ? 0x100u : 0xE0u);
    const auto mmucr = bus_.read32(0xFF000010u);
    auto register_access = [&](std::uint32_t destination) {
        if ((destination & 0xFFFFFFC0u) == 0xFF000000u && size != 4)
            fault(a, write ? 0x100u : 0xE0u);
        if (write && destination >= 0xF6000000u && destination < 0xF7000000u &&
            (destination & 0x80u))
            throw std::runtime_error(
                "associative UTLB writes are not implemented in strict MMU mode");
        return destination;
    };
    const bool sq = a >= 0xE0000000u && a < 0xE4000000u;
    if (!privileged && a >= 0x80000000u && !(sq && !fetch && !(mmucr & 0x200u)))
        fault(a, write ? 0x100u : 0xE0u);
    if (fetch && a >= 0xE0000000u)
        fault(a, 0xE0u);
    if (sq) {
        // SH7750 queues accept longword/quadword writes, not ordinary data reads. Access
        // rights and D are checked when filling the queue; PREF uses read-type checks.
        if (!write)
            fault(a, 0xE0u);
        if (size != 4)
            fault(a, 0x100u);
        if (mmucr & 1u)
            (void)page(a, true);
        return a;
    }
    if (a >= 0xE0000000u)
        return register_access(a);
    if (a >= 0x7C000000u && a < 0x80000000u) {
        if (fetch || !(bus_.read32(0xFF00001Cu) & 0x20u))
            fault(a, write ? 0x100u : 0xE0u);
        return a;  // on-chip RAM is outside the TLB
    }
    if (a >= 0x80000000u && a < 0xC0000000u)
        return a;
    if (!(mmucr & 1u))
        return a;
    // P1 alias prevents a physical destination from being interpreted as on-chip virtual RAM.
    const auto pa = page(a, write);
    // The MMU can expose on-chip peripherals to user code through a protected mapping.
    return register_access((pa >= 0x1C000000u ? 0xE0000000u : 0x80000000u) | pa);
}

std::uint8_t MmuMemory::read8(std::uint32_t a) {
    return bus_.read8(translate(a, 1, false));
}
std::uint16_t MmuMemory::read16(std::uint32_t a) {
    return bus_.read16(translate(a, 2, false));
}
std::uint16_t MmuMemory::fetch16(std::uint32_t a) {
    return bus_.read16(translate(a, 2, false, true));
}
std::uint32_t MmuMemory::read32(std::uint32_t a) {
    return bus_.read32(translate(a, 4, false));
}
std::uint64_t MmuMemory::read64(std::uint32_t a) {
    if (a > 0xFFFFFFFBu)
        fault(a, 0xE0u);
    const auto lo = translate(a, 4, false), hi = translate(a + 4, 4, false);
    return bus_.read32(lo) | (static_cast<std::uint64_t>(bus_.read32(hi)) << 32);
}
void MmuMemory::write8(std::uint32_t a, std::uint8_t v) {
    bus_.write8(translate(a, 1, true), v);
}
void MmuMemory::write16(std::uint32_t a, std::uint16_t v) {
    bus_.write16(translate(a, 2, true), v);
}
void MmuMemory::write32(std::uint32_t a, std::uint32_t v) {
    bus_.write32(translate(a, 4, true), v);
}
void MmuMemory::write64(std::uint32_t a, std::uint64_t v) {
    if (a > 0xFFFFFFFBu)
        fault(a, 0x100u);
    const auto lo = translate(a, 4, true), hi = translate(a + 4, 4, true);
    bus_.write32(lo, static_cast<std::uint32_t>(v));
    bus_.write32(hi, static_cast<std::uint32_t>(v >> 32));
}
void MmuMemory::sq_write32(std::uint32_t a, std::uint32_t v) {
    if (a < 0xE0000000u || a >= 0xE4000000u)
        fault(a, 0x100u);
    bus_.sq_write32(translate(a, 4, true), v);
}
void MmuMemory::sq_flush(std::uint32_t a) {
    if (a < 0xE0000000u || a >= 0xE4000000u)
        return;
    const auto mmucr = bus_.read32(0xFF000010u);
    if ((a & 3u) || (!(cpu_.sr & sh4::SR_MD) && (mmucr & 0x200u)))
        fault(a, 0xE0u);
    a &= ~31u;
    if (mmucr & 1u) {
        const auto dest = page(a, false);  // PREF has read-type checks, no QACR fallback
        bus_.sq_flush_to(a, dest);
        ++bus_.sq_tlb_flushes;
    } else {
        bus_.sq_flush(a);
    }
}
void MmuMemory::ldtlb() {
    if (!(cpu_.sr & sh4::SR_MD))
        throw std::runtime_error("privileged LDTLB in user mode");
    bus_.ldtlb();
}
}  // namespace dream::mem
