#pragma once
#include <algorithm>

#include "dream/runtime/wince/pe_loader.h"

namespace wince_test {
inline void p16(std::vector<std::uint8_t>& b, std::size_t p, std::uint16_t v) {
    b.at(p) = static_cast<std::uint8_t>(v);
    b.at(p + 1) = static_cast<std::uint8_t>(v >> 8);
}
inline void p32(std::vector<std::uint8_t>& b, std::size_t p, std::uint32_t v) {
    p16(b, p, static_cast<std::uint16_t>(v));
    p16(b, p + 2, static_cast<std::uint16_t>(v >> 16));
}
inline void text(std::vector<std::uint8_t>& b, std::size_t p, const std::string& s) {
    std::copy(s.begin(), s.end(), b.begin() + p);
    b.at(p + s.size()) = 0;
}
// Synthetic SH-4 DLL/EXE with one named export, optional import and a relocatable literal.
// DLL entry returns TRUE; its Work export is the same function. No proprietary binaries.
inline std::vector<std::uint8_t> module(bool dll = true, const std::string& dependency = "",
                                        const std::string& forward = "",
                                        std::uint32_t base = 0x10000) {
    std::vector<std::uint8_t> b(0x1000);
    p16(b, 0, 0x5A4D);
    p32(b, 0x3C, 0x80);
    p32(b, 0x80, 0x4550);
    p16(b, 0x84, 0x1A6);
    p16(b, 0x86, 1);
    p16(b, 0x94, 0xE0);
    p16(b, 0x96, dll ? 0x2102 : 0x102);
    p16(b, 0x98, 0x10B);
    p32(b, 0xA8, 0x1000);
    p32(b, 0xB4, base);
    p32(b, 0xB8, 0x1000);
    p32(b, 0xBC, 0x200);
    p32(b, 0xD0, 0x2000);
    p32(b, 0xD4, 0x200);
    p16(b, 0xDC, 9);
    p32(b, 0xF4, 16);
    text(b, 0x178, ".text");
    p32(b, 0x180, 0x1000);
    p32(b, 0x184, 0x1000);
    p32(b, 0x188, 0xE00);
    p32(b, 0x18C, 0x200);
    p32(b, 0x19C, 0x60000020);
    p16(b, 0x200, 0xE001);
    p16(b, 0x202, 0x000B);
    p16(b, 0x204, 0x0009);
    p32(b, 0x210, base + 0x1000);
    p32(b, 0x120, 0x1500);
    p32(b, 0x124, 12);
    p32(b, 0x700, 0x1000);
    p32(b, 0x704, 12);
    p16(b, 0x708, 0x3010);
    if (!dependency.empty()) {
        p32(b, 0x100, 0x1200);
        p32(b, 0x104, 40);
        p32(b, 0x400, 0x1240);
        p32(b, 0x40C, 0x1280);
        p32(b, 0x410, 0x1260);
        p32(b, 0x440, 0x12C0);
        p32(b, 0x460, 0x12C0);
        text(b, 0x480, dependency);
        text(b, 0x4C2, "Work");
    }
    p32(b, 0xF8, 0x1400);
    p32(b, 0xFC, 0x100);
    p32(b, 0x610, 1);
    p32(b, 0x614, 1);
    p32(b, 0x618, 1);
    p32(b, 0x61C, 0x1440);
    p32(b, 0x620, 0x1450);
    p32(b, 0x624, 0x1460);
    p32(b, 0x640, forward.empty() ? 0x1000 : 0x1480);
    p32(b, 0x650, 0x1470);
    text(b, 0x670, "Work");
    if (!forward.empty())
        text(b, 0x680, forward);
    return b;
}
}  // namespace wince_test
