#include <array>
#include <bit>
#include <limits>

#include "dream/runtime/wince/directx.h"

#include "doctest.h"
#include "wince_fixture.h"
#ifdef DREAM_DEV_INTERPRETER
#include "dream/runtime/devinterp/interpreter.h"
#endif

using namespace dream::wince;
namespace {
struct Guest {
    ProcessMemory memory;
    DirectX dx{memory};
    dream::sh4::Ctx cpu{};
    std::uint32_t data = memory.allocate(16384);
    Guest() {
        cpu.r[15] = data + 16000;
        for (unsigned i = 0; i < 4; ++i) memory.write32(cpu.r[15] + i * 4, 0xBAD0BAD0u + i);
    }
    std::uint32_t p(unsigned offset) const { return data + offset; }
    std::uint32_t invoke(std::uint32_t target, std::initializer_list<std::uint32_t> args) {
        unsigned i = 0;
        for (const auto a : args) {
            if (i < 4)
                cpu.r[4 + i] = a;
            else
                memory.write32(cpu.r[15] + i * 4, a);
            ++i;
        }
        REQUIRE(dx.invoke(cpu, memory, target));
        return cpu.r[0];
    }
    std::uint32_t method(std::uint32_t object, unsigned slot,
                         std::initializer_list<std::uint32_t> args = {}) {
        cpu.r[4] = object;
        unsigned i = 1;
        for (const auto a : args) {
            if (i < 4)
                cpu.r[4 + i] = a;
            else
                memory.write32(cpu.r[15] + i * 4, a);
            ++i;
        }
        const auto target = memory.read32(memory.read32(object) + slot * 4);
        REQUIRE(dx.invoke(cpu, memory, target));
        return cpu.r[0];
    }
    void guid(unsigned offset, std::array<std::uint32_t, 4> g) {
        for (unsigned i = 0; i < 4; ++i) memory.write32(p(offset + i * 4), g[i]);
    }
    std::uint32_t draw(bool v4 = false) {
        const auto trap =
            dx.resolve(Import{"ddraw.dll", v4 ? "DirectDrawCreateEx" : "DirectDrawCreate"});
        REQUIRE(trap.has_value());
        if (v4) {
            guid(32, {0x9C59509A, 0x11D139BD, 0xC0004A8C, 0xC530D94F});
            REQUIRE(invoke(*trap, {0, p(0), p(32), 0}) == 0);
        } else
            REQUIRE(invoke(*trap, {0, p(0), 0}) == 0);
        const auto d = memory.read32(p(0));
        REQUIRE(method(d, 21, {8, 8, 32, 0, 0}) == 0);
        return d;
    }
    std::uint32_t surface(std::uint32_t draw, std::uint32_t caps, bool v4 = false,
                          bool flip = false) {
        memory.write_bytes(p(128), std::vector<std::uint8_t>(124));
        memory.write32(p(128), v4 ? 124 : 108);
        memory.write32(p(132), flip ? 0x21 : 1);
        memory.write32(p(148), flip ? 1 : 0);
        memory.write32(p(232), caps);
        REQUIRE(method(draw, 6, {p(128), p(4), 0}) == 0);
        return memory.read32(p(4));
    }
};
constexpr std::uint32_t invalid = 0x80070057, unsupported = 0x80004001;
}  // namespace
TEST_CASE("WinCE DirectDraw COM: guest surface locks, flip chain, fill and read-only snapshot") {
    Guest g;
    const auto draw = g.draw();
    const auto front = g.surface(draw, 0x218, false, true);
    g.memory.write32(g.p(256), 4);
    REQUIRE(g.method(front, 12, {g.p(256), g.p(8)}) == 0);
    const auto back = g.memory.read32(g.p(8));
    g.memory.write32(g.p(384), 108);
    REQUIRE(g.method(back, 25, {0, g.p(384), 1, 0}) == 0);
    const auto pixels = g.memory.read32(g.p(420));
    CHECK(g.memory.read32(g.p(400)) == 32);
    g.memory.write32(pixels, 0x00FF0000);
    CHECK(g.method(front, 11, {0, 1}) == 0x887601AE);
    CHECK(g.method(back, 32, {pixels + 4}) == invalid);
    REQUIRE(g.method(back, 32, {pixels}) == 0);
    REQUIRE(g.method(front, 11, {0, 1}) == 0);
    REQUIRE(g.dx.frame().size() == 64);
    CHECK(g.dx.frame()[0] == 0xFFFF0000);
    // Read-only locking immediately after Flip must preserve the current guest pixels.
    REQUIRE(g.method(front, 25, {0, g.p(384), 0x10, 0}) == 0);
    const auto fp = g.memory.read32(g.p(420));
    CHECK(g.memory.read32(fp) == 0x00FF0000);
    g.memory.write32(fp, 0xFF);  // Writes through a READONLY lock are discarded.
    REQUIRE(g.method(front, 32, {fp}) == 0);
    CHECK(g.dx.frame()[0] == 0xFFFF0000);
    g.memory.write32(g.p(512), 100);
    g.memory.write32(g.p(592), 0x0000FF00);
    REQUIRE(g.method(back, 5, {0, 0, 0, 0x1000400, g.p(512)}) == 0);
    REQUIRE(g.method(front, 11, {0, 1}) == 0);
    CHECK(g.dx.frame()[0] == 0xFF00FF00);
    CHECK(g.dx.frame()[63] == 0xFF00FF00);
    CHECK(g.method(back, 5, {0, 0, 0, 0x20000, 0}) == unsupported);
    CHECK_FALSE(g.dx.diagnostic().empty());
    CHECK(g.method(back, 7, {9, 0, front, 0, 0}) == invalid);
    CHECK(g.method(front, 5, {0, back, 0, 0x1000000, 0}) == 0);
    CHECK(g.dx.width() == 8);
    CHECK(g.dx.height() == 8);
}
TEST_CASE("WinCE DirectX COM: identity reference lifetime and unsupported interfaces") {
    Guest g;
    const auto draw = g.draw();
    g.guid(32, {0x9C59509A, 0x11D139BD, 0xC0004A8C, 0xC530D94F});
    REQUIRE(g.method(draw, 0, {g.p(32), g.p(4)}) == 0);
    const auto draw4 = g.memory.read32(g.p(4));
    CHECK(draw4 != draw);
    g.guid(32, {0, 0, 0xC0, 0x46000000});
    REQUIRE(g.method(draw4, 0, {g.p(32), g.p(4)}) == 0);
    CHECK(g.memory.read32(g.p(4)) == draw);
    g.guid(32, {123, 456, 789, 0});
    CHECK(g.method(draw, 0, {g.p(32), g.p(4)}) == 0x80004002);
    CHECK(g.memory.read32(g.p(4)) == 0);
    CHECK(g.method(draw, 0, {g.p(32), 0}) == 0x80004003);
    CHECK(g.method(draw, 2) == 2);
    CHECK(g.method(draw4, 2) == 1);
    const auto release = g.memory.read32(g.memory.read32(draw) + 8);
    CHECK(g.method(draw, 2) == 0);
    CHECK(g.invoke(release, {draw}) == invalid);
    CHECK_THROWS_AS(g.memory.read32(draw), std::runtime_error);
}
TEST_CASE("WinCE Direct3D3 COM: indexed TL triangles, validation, culling and presentation") {
    Guest g;
    const auto draw = g.draw(true), target = g.surface(draw, 0x2200, true);
    g.guid(32, {0xBB223240, 0x11D0E72B, 0xAA00B4A9, 0x3E99C000});
    REQUIRE(g.method(draw, 0, {g.p(32), g.p(8)}) == 0);
    const auto d3d = g.memory.read32(g.p(8));
    g.guid(32, {0xA4665C60, 0x11CF2673, 0xAA001AA3, 0x5633B900});
    REQUIRE(g.method(d3d, 8, {g.p(32), target, g.p(12), 0}) == 0);
    const auto device = g.memory.read32(g.p(12));
    REQUIRE(g.method(device, 9) == 0);
    const float xy[][2]{{0, 0}, {8, 0}, {0, 8}};
    for (unsigned i = 0; i < 3; ++i) {
        const auto p = g.p(1024 + i * 32);
        g.memory.write32(p, std::bit_cast<std::uint32_t>(xy[i][0]));
        g.memory.write32(p + 4, std::bit_cast<std::uint32_t>(xy[i][1]));
        g.memory.write32(p + 8, std::bit_cast<std::uint32_t>(0.5f));
        g.memory.write32(p + 12, std::bit_cast<std::uint32_t>(1.0f));
        g.memory.write32(p + 16, 0xFFFF0000);
        g.memory.write16(g.p(1200 + i * 2), static_cast<std::uint16_t>(i));
    }
    CHECK(g.method(device, 29, {4, 0x1C4, g.p(1024), 3, g.p(1200), 3, 0}) == 0);
    g.memory.write16(g.p(1204), 3);
    CHECK(g.method(device, 29, {4, 0x1C4, g.p(1024), 3, g.p(1200), 3, 0}) == invalid);
    CHECK(g.method(device, 28, {4, 0x112, g.p(1024), 3, 0}) == unsupported);
    CHECK(g.method(device, 38, {0, target}) == unsupported);
    CHECK(g.method(device, 22, {7, 1}) == unsupported);  // No fake Z-buffer support.
    // Cull a second differently colored triangle; it must not modify the target.
    CHECK(g.method(device, 22, {22, 2}) == 0);
    for (unsigned i = 0; i < 3; ++i) g.memory.write32(g.p(1040 + i * 32), 0xFF00FF00);
    CHECK(g.method(device, 28, {4, 0x1C4, g.p(1024), 3, 0}) == 0);
    g.memory.write32(g.p(1024),
                     std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity()));
    CHECK(g.method(device, 28, {4, 0x1C4, g.p(1024), 3, 0}) == invalid);
    CHECK(g.method(device, 10) == 0);
    g.memory.write32(g.p(384), 124);
    REQUIRE(g.method(target, 25, {0, g.p(384), 0x10, 0}) == 0);
    REQUIRE(g.method(target, 32, {0}) == 0);
    REQUIRE(g.dx.frame().size() == 64);
    CHECK(g.dx.frame()[9] == 0xFFFF0000);
    CHECK(g.dx.frame()[63] == 0xFF000000);
}
TEST_CASE("WinCE DirectSound COM: wrapping PCM lock, playback, gain, cursor and release") {
    Guest g;
    const auto factory = g.dx.resolve(Import{"dsound.dll", "DirectSoundCreate"});
    REQUIRE(factory.has_value());
    REQUIRE(g.invoke(*factory, {0, g.p(0), 0}) == 0);
    const auto sound = g.memory.read32(g.p(0));
    g.memory.write32(g.p(128), 20);
    g.memory.write32(g.p(132), 0xA0);
    g.memory.write32(g.p(136), 8);
    g.memory.write32(g.p(144), g.p(256));
    g.memory.write16(g.p(256), 1);
    g.memory.write16(g.p(258), 1);
    g.memory.write32(g.p(260), 44100);
    g.memory.write32(g.p(264), 88200);
    g.memory.write16(g.p(268), 2);
    g.memory.write16(g.p(270), 16);
    g.memory.write16(g.p(272), 0);
    REQUIRE(g.method(sound, 3, {g.p(128), g.p(4), 0}) == 0);
    const auto buffer = g.memory.read32(g.p(4));
    REQUIRE(g.method(buffer, 11, {6, 8, g.p(16), g.p(20), g.p(24), g.p(28), 0}) == 0);
    const auto p1 = g.memory.read32(g.p(16)), p2 = g.memory.read32(g.p(24));
    CHECK(g.memory.read32(g.p(20)) == 2);
    CHECK(g.memory.read32(g.p(28)) == 6);
    g.memory.write16(p1, 4000);
    g.memory.write16(p2, 1000);
    g.memory.write16(p2 + 2, 2000);
    g.memory.write16(p2 + 4, 3000);
    CHECK(g.method(buffer, 12, {0, 0, 1}) == 0x88780032);
    CHECK(g.method(buffer, 19, {p1, 2, p2, 4}) == invalid);
    REQUIRE(g.method(buffer, 19, {p1, 2, p2, 6}) == 0);
    REQUIRE(g.method(buffer, 12, {0, 0, 1}) == 0);
    std::array<std::int16_t, 10> pcm{};
    g.dx.mix(pcm);
    CHECK(pcm ==
          std::array<std::int16_t, 10>{1000, 1000, 2000, 2000, 3000, 3000, 4000, 4000, 1000, 1000});
    REQUIRE(g.method(buffer, 4, {g.p(32), g.p(36)}) == 0);
    CHECK(g.memory.read32(g.p(32)) == 2);
    REQUIRE(g.method(buffer, 15, {static_cast<std::uint32_t>(-10000)}) == 0);
    g.dx.mix(pcm);
    CHECK(std::all_of(pcm.begin(), pcm.end(), [](auto v) { return v == 0; }));
    CHECK(g.method(buffer, 17, {22050}) == 0);
    CHECK(g.method(buffer, 8, {g.p(32)}) == 0);
    CHECK(g.memory.read32(g.p(32)) == 22050);
    CHECK(g.method(buffer, 18) == 0);
    CHECK(g.method(buffer, 9, {g.p(32)}) == 0);
    CHECK(g.memory.read32(g.p(32)) == 0);
    CHECK(g.method(buffer, 16, {0}) == unsupported);
    CHECK(g.method(sound, 2) == 0);  // Buffer keeps its backing owner alive.
    CHECK(g.method(buffer, 2) == 0);
    CHECK_THROWS_AS(g.memory.read32(p1), std::runtime_error);
    g.dx.mix(pcm);
    CHECK(std::all_of(pcm.begin(), pcm.end(), [](auto v) { return v == 0; }));
}
#ifdef DREAM_DEV_INTERPRETER
TEST_CASE("WinCE DirectX execution: PE import, JSR vtable and SH4 delay-slot arguments") {
    Guest g;
    auto file = wince_test::module(false, "DDRAW.DLL");
    wince_test::text(file, 0x4C2, "DirectDrawCreate");
    // Create a guest object through the IAT, then call AddRef through its guest vtable.
    // r5 is set in the factory JSR delay slot; no host-side method invocation is used.
    const std::uint16_t code[]{0x4F22, 0x7FF0, 0xD20A, 0x6222, 0xE400, 0xE600,
                               0x420B, 0x6593, 0x6492, 0x6242, 0x5221, 0x420B,
                               0x0009, 0x7F10, 0x4F26, 0x000B, 0x0009};
    for (unsigned i = 0; i < std::size(code); ++i) wince_test::p16(file, 0x200 + i * 2, code[i]);
    wince_test::p32(file, 0x230, 0x11260);
    wince_test::p16(file, 0x708, 0x3030);
    const auto im = load_pe(file, {}, [&](const Import& i) { return g.dx.resolve(i); });
    g.memory.install(im);
    g.cpu.r[9] = g.p(0);
    g.cpu.pc = im.entry;
    g.cpu.pr = 0x1EFFFFFC;
    g.cpu.next_event = ~0ull;
    dream::devinterp::Options options;
    options.call_translated = false;
    options.max_instructions = 100;
    options.hle_call = [&](auto& c, auto& m, auto target) { return g.dx.invoke(c, m, target); };
    dream::devinterp::Stats stats;
    CHECK(dream::devinterp::run(g.cpu, g.memory, g.cpu.pr, options, &stats) ==
          dream::devinterp::Stop::Returned);
    CHECK(g.cpu.r[0] == 2);
    CHECK(stats.native_calls == 2);
    CHECK(g.memory.read32(g.p(0)) != 0);
}
#endif
