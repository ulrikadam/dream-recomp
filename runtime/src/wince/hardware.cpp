#include "dream/runtime/wince/hardware.h"

namespace dream::wince {
namespace {
// On-chip registers seen through P4 (0xFF......) or the area-7 alias (0x1F......).
constexpr std::uint32_t kPctra = 0x1F80002C, kPdtra = 0x1F800030;
std::uint32_t area7(std::uint32_t a) {
    return a & 0x1FFFFFFFu;
}
}  // namespace

bool DreamcastHardware::read(std::uint32_t address, unsigned size, std::uint64_t& value) {
    const auto a = area7(address);
    if (a == kPctra && size == 4) {
        value = pctra_;
        return true;
    }
    if (a == kPdtra && size == 2) {
        // The cable type sits in bits 8-9; the low bits answer the probe the BIOS and the
        // libraries make by driving pins through PCTRA (the nullDC/Flycast reading).
        std::uint32_t low = 0;
        if ((pctra_ & 0xF) == 0x8 || (pctra_ & 0xF) == 0xB)
            low = 3;
        if ((pctra_ & 0xF) == 0xB && (pdtra_ & 0xF) == 2)
            low = 0;
        else if ((pctra_ & 0xF) == 0xC && (pdtra_ & 0xF) == 2)
            low = 3;
        value = low | static_cast<std::uint32_t>(cable_) << 8;
        return true;
    }
    return false;
}

bool DreamcastHardware::write(std::uint32_t address, unsigned size, std::uint64_t value) {
    const auto a = area7(address);
    if (a == kPctra && size == 4) {
        pctra_ = static_cast<std::uint32_t>(value);
        return true;
    }
    if (a == kPdtra && size == 2) {
        pdtra_ = static_cast<std::uint32_t>(value);
        return true;
    }
    return false;
}
}  // namespace dream::wince
