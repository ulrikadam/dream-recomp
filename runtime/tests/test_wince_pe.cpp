#include <algorithm>
#include <array>
#include <string>

#include "dream/runtime/wince/process.h"

#include "doctest.h"
#ifdef DREAM_DEV_INTERPRETER
#include "dream/runtime/devinterp/interpreter.h"
#endif

using namespace dream::wince;
namespace {
void p16(std::vector<std::uint8_t>& b, std::size_t p, std::uint16_t v) {
    b.at(p) = static_cast<std::uint8_t>(v);
    b.at(p + 1) = static_cast<std::uint8_t>(v >> 8);
}
void p32(std::vector<std::uint8_t>& b, std::size_t p, std::uint32_t v) {
    p16(b, p, static_cast<std::uint16_t>(v));
    p16(b, p + 2, static_cast<std::uint16_t>(v >> 16));
}
std::uint32_t word(const Image& im, std::size_t rva) {
    return im.bytes.at(rva) | (static_cast<std::uint32_t>(im.bytes.at(rva + 1)) << 8) |
           (static_cast<std::uint32_t>(im.bytes.at(rva + 2)) << 16) |
           (static_cast<std::uint32_t>(im.bytes.at(rva + 3)) << 24);
}
void string(std::vector<std::uint8_t>& b, std::size_t p, const std::string& s) {
    std::copy(s.begin(), s.end(), b.begin() + p);
    b.at(p + s.size()) = 0;
}
// A complete, hand-built PE32 application. No SDK, OS or game binary required.
std::vector<std::uint8_t> fixture(bool imports = true) {
    std::vector<std::uint8_t> b(0x800);
    p16(b, 0, 0x5A4D);
    p32(b, 0x3C, 0x80);
    p32(b, 0x80, 0x4550);
    p16(b, 0x84, 0x1A6);
    p16(b, 0x86, 1);
    p16(b, 0x94, 0xE0);
    p16(b, 0x96, 0x102);
    p16(b, 0x98, 0x10B);
    p32(b, 0xA8, 0x1000);
    p32(b, 0xB4, 0x10000);
    p32(b, 0xB8, 0x1000);
    p32(b, 0xBC, 0x200);
    p32(b, 0xD0, 0x2000);
    p32(b, 0xD4, 0x200);
    p16(b, 0xDC, 9);
    p32(b, 0xF4, 16);
    if (imports) {
        p32(b, 0x100, 0x1100);
        p32(b, 0x104, 40);
    }
    p32(b, 0x120, 0x1300);
    p32(b, 0x124, 12);
    string(b, 0x178, ".text");
    p32(b, 0x180, 0x1000);
    p32(b, 0x184, 0x1000);
    p32(b, 0x188, 0x600);
    p32(b, 0x18C, 0x200);
    p32(b, 0x19C, 0x60000020);
    // Save PR, call GetTickCount through the IAT, restore PR and return.
    const std::uint16_t code[] = {0x4F22, 0x7FF0, 0xD204, 0x6222, 0x420B,
                                  0x0009, 0x7F10, 0x4F26, 0x000B, 0x0009};
    for (std::size_t i = 0; i < std::size(code); ++i) p16(b, 0x200 + i * 2, code[i]);
    p32(b, 0x218, 0x11190);
    p32(b, 0x240, 0x11080);
    p32(b, 0x300, 0x1180);
    p32(b, 0x30C, 0x1150);
    p32(b, 0x310, 0x1190);
    string(b, 0x350, "COREDLL.DLL");
    p32(b, 0x380, 0x11A0);
    p32(b, 0x390, 0x11A0);
    string(b, 0x3A2, "GetTickCount");
    p32(b, 0x500, 0x1000);
    p32(b, 0x504, 12);
    p16(b, 0x508, 0x3018);
    p16(b, 0x50A, 0x3040);
    return b;
}
Image load(const std::vector<std::uint8_t>& b, std::uint32_t base = 0x10000) {
    Kernel kernel;
    return load_pe(b, base, [&](const Import& i) { return kernel.resolve(i); });
}
}  // namespace

TEST_CASE("WinCE PE: sections BSS entry relocations and named imports") {
    const auto image = load(fixture(), 0x20000);
    CHECK(image.base == 0x20000);
    CHECK(image.entry == 0x21000);
    CHECK_FALSE(image.dll);
    REQUIRE(image.sections.size() == 1);
    CHECK(image.sections[0].name == ".text");
    REQUIRE(image.imports.size() == 1);
    CHECK(image.imports[0].name == "GetTickCount");
    CHECK(image.imports[0].dll == "COREDLL.DLL");
    CHECK(word(image, 0x1190) == Kernel::kTrapBase);
    CHECK(word(image, 0x1018) == 0x21190);
    CHECK(word(image, 0x1040) == 0x21080);
    CHECK(image.bytes.size() == 0x2000);
    CHECK(image.bytes[0x1FFF] == 0);
    CHECK(word(load(fixture()), 0x1040) == 0x11080);
}

TEST_CASE("WinCE PE: imports by ordinal and OriginalFirstThunk fallback") {
    auto b = fixture();
    p32(b, 0x300, 0);
    p32(b, 0x390, 0x80000007);
    CHECK_THROWS_WITH_AS(load(b), "unresolved import: COREDLL.DLL!#7", PeError);
    Kernel k;
    k.bind_ordinal(7, "GetTickCount");
    auto image = load_pe(b, {}, [&](const Import& i) { return k.resolve(i); });
    CHECK(image.imports[0].ordinal == 7);
    CHECK(word(image, 0x1190) == Kernel::kTrapBase);
    CHECK_THROWS_AS(k.bind_ordinal(7, "Sleep"), std::invalid_argument);
    CHECK_THROWS_AS(k.bind_ordinal(8, "DoesNotExist"), std::invalid_argument);
}

TEST_CASE("WinCE PE: malformed executable headers and sections are rejected") {
    auto b = fixture(false);
    SUBCASE("short DOS header") {
        b.resize(10);
    }
    SUBCASE("oversized PE offset") {
        p32(b, 0x3C, 0xFFFFFFF0);
    }
    SUBCASE("wrong architecture") {
        p16(b, 0x84, 0x14C);
    }
    SUBCASE("wrong subsystem") {
        p16(b, 0xDC, 3);
    }
    SUBCASE("PE32 plus") {
        p16(b, 0x98, 0x20B);
    }
    SUBCASE("non executable") {
        p16(b, 0x96, 0);
    }
    SUBCASE("huge image") {
        p32(b, 0xD0, 0x7FFFF000);
    }
    SUBCASE("section outside image") {
        p32(b, 0x184, 0x2000);
    }
    SUBCASE("raw data truncated") {
        b.resize(0x700);
    }
    SUBCASE("raw data aliases headers") {
        p32(b, 0x18C, 0);
    }
    SUBCASE("directory in hole") {
        p32(b, 0x120, 0x800);
    }
    SUBCASE("excess directory count") {
        p32(b, 0xF4, 1000);
    }
    SUBCASE("entry outside code") {
        p32(b, 0xA8, 0x800);
    }
    SUBCASE("unaligned entry") {
        p32(b, 0xA8, 0x1001);
    }
    SUBCASE("non executable section") {
        p32(b, 0x19C, 0xC0000040);
    }
    SUBCASE("bad section alignment") {
        p32(b, 0xB8, 3);
    }
    CHECK_THROWS_AS(load(b), PeError);
}

TEST_CASE("WinCE PE: malformed relocations and unsupported initialization fail explicitly") {
    auto b = fixture(false);
    SUBCASE("zero relocation block") {
        p32(b, 0x504, 0);
    }
    SUBCASE("truncated block") {
        p32(b, 0x504, 16);
    }
    SUBCASE("unaligned page") {
        p32(b, 0x500, 0x1001);
    }
    SUBCASE("unsupported type") {
        p16(b, 0x508, 0x5018);
    }
    SUBCASE("outside target") {
        p32(b, 0x500, 0xFFFFF000);
    }
    SUBCASE("missing HIGHADJ pair") {
        p16(b, 0x50A, 0x4040);
    }
    SUBCASE("stripped rebasing") {
        p16(b, 0x96, 0x103);
    }
    SUBCASE("TLS initialization") {
        p32(b, 0x140, 0x1400);
        p32(b, 0x144, 24);
    }
    CHECK_THROWS_AS(load(b, 0x20000), PeError);
}

TEST_CASE("WinCE PE: import corruption is not reported as a successful load") {
    auto b = fixture();
    SUBCASE("unknown function") {
        string(b, 0x3A2, "UnknownCall");
    }
    SUBCASE("wrong DLL for COREDLL function") {
        string(b, 0x350, "DDRAW.DLL");
    }
    SUBCASE("unmapped thunk") {
        p32(b, 0x380, 0xF000000);
    }
    SUBCASE("reserved ordinal bits") {
        p32(b, 0x380, 0x80010007);
    }
    SUBCASE("unterminated descriptors") {
        p32(b, 0x104, 20);
    }
    SUBCASE("empty DLL") {
        b[0x350] = 0;
    }
    SUBCASE("IAT outside image") {
        p32(b, 0x310, 0xFFFFFFFC);
    }
    CHECK_THROWS_AS(load(b), PeError);
}

TEST_CASE("WinCE process: image protections allocation reuse and atomic writes") {
    ProcessMemory memory;
    const auto image = load(fixture());
    memory.install(image);
    CHECK(memory.fetch16(image.entry) == 0x4F22);
    CHECK_THROWS_AS(memory.write16(image.entry, 0), std::runtime_error);
    CHECK_THROWS_AS(memory.fetch16(image.base), std::runtime_error);
    CHECK_THROWS_AS(memory.install(image), std::runtime_error);
    const auto p = memory.allocate(4096);
    REQUIRE(p != 0);
    CHECK(memory.read32(p) == 0);
    memory.write32(p + 4092, 123);
    CHECK_THROWS_AS(memory.write64(p + 4092, ~0ull), std::runtime_error);
    CHECK(memory.read32(p + 4092) == 123);
    CHECK_THROWS_AS(memory.fetch16(p), std::runtime_error);
    CHECK_FALSE(memory.release(p + 4));
    CHECK(memory.release(p));
    CHECK_FALSE(memory.release(p));
    CHECK(memory.allocate(4096) == p);
    CHECK(memory.read32(p + 4092) == 0);
    CHECK(memory.allocate(0xFFFFFFFF) == 0);
}

TEST_CASE("WinCE COREDLL: heap errors TLS clock and exit") {
    Kernel k;
    ProcessMemory memory;
    dream::sh4::Ctx c{};
    auto call = [&](const std::string& name, std::uint32_t a = 0, std::uint32_t b = 0) {
        Import imp;
        imp.dll = "coredll.dll";
        imp.name = name;
        const auto target = k.resolve(imp);
        REQUIRE(target.has_value());
        c.r[4] = a;
        c.r[5] = b;
        const bool invoked = k.invoke(c, memory, *target);
        REQUIRE(invoked);
        return c.r[0];
    };
    call("SetLastError", 123);
    CHECK(call("GetLastError") == 123);
    const auto p = call("LocalAlloc", 0x40, 100);
    REQUIRE(p != 0);
    CHECK(memory.read32(p) == 0);
    CHECK(call("LocalFree", p + 4) == p + 4);
    CHECK(call("GetLastError") == 6);
    CHECK(call("LocalFree", p) == 0);
    CHECK(call("LocalAlloc", 2, 100) == 0);
    CHECK(call("GetLastError") == 87);
    const auto slot = call("TlsAlloc");
    CHECK(slot == 0);
    CHECK(call("TlsSetValue", slot, 0xCAFE) == 1);
    CHECK(call("TlsGetValue", slot) == 0xCAFE);
    CHECK(call("GetLastError") == 0);
    CHECK(call("TlsFree", slot) == 1);
    CHECK(call("TlsGetValue", slot) == 0);
    CHECK(call("GetLastError") == 87);
    CHECK(call("TlsAlloc") == 0);
    call("Sleep", 25);
    CHECK(call("GetTickCount") == 25);
    CHECK_THROWS_AS(call("Sleep", 0xFFFFFFFF), std::runtime_error);
    CHECK_THROWS_AS(call("ExitProcess", 42), ProcessExit);
    CHECK_FALSE(k.invoke(c, memory, Kernel::kTrapBase + 1));
}

#ifdef DREAM_DEV_INTERPRETER
TEST_CASE("WinCE execution: rebased PE calls imported COREDLL and returns through SH4 ABI") {
    Kernel kernel;
    const auto image =
        load_pe(fixture(), 0x20000, [&](const Import& i) { return kernel.resolve(i); });
    ProcessMemory memory;
    memory.install(image);
    dream::sh4::Ctx c{};
    c.pc = image.entry;
    c.pr = 0x1EFFFFFC;
    c.r[15] = memory.allocate(4096) + 4096;
    c.cycles = 123u * 200000u;
    c.next_event = ~0ull;
    dream::devinterp::Options options;
    options.call_translated = false;
    options.max_instructions = 32;
    options.hle_call = [&](auto& cpu, auto& mem, auto pc) { return kernel.invoke(cpu, mem, pc); };
    dream::devinterp::Stats stats;
    CHECK(dream::devinterp::run(c, memory, c.pr, options, &stats) ==
          dream::devinterp::Stop::Returned);
    CHECK(c.r[0] == 123);
    CHECK(stats.native_calls == 1);
    CHECK(stats.instructions == 10);
}
#endif
