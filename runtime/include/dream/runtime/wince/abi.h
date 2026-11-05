#pragma once
#include <stdexcept>

#include "dream/runtime/memory.h"
#include "dream/runtime/sh4/ctx.h"

namespace dream::wince {
// Windows CE SuperH ABI: R4..R7 followed by SP+16, SP+20, ... . The caller reserves
// all four register home slots even for a function with fewer than four parameters.
inline std::uint32_t argument(const sh4::Ctx& cpu, Memory& memory, unsigned index) {
    if (index < 4)
        return cpu.r[4 + index];
    const auto p = static_cast<std::uint64_t>(cpu.r[15]) + static_cast<std::uint64_t>(index) * 4;
    if (p >= 0x80000000ull)
        throw std::runtime_error("WinCE stack argument address overflow");
    return memory.read32(static_cast<std::uint32_t>(p));
}
}  // namespace dream::wince
