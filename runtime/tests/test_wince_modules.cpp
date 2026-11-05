#include "dream/runtime/wince/modules.h"

#include "doctest.h"
#include "wince_fixture.h"
#ifdef DREAM_DEV_INTERPRETER
#include "dream/runtime/devinterp/interpreter.h"
#endif

using namespace dream::wince;
using namespace wince_test;
namespace {
struct Harness {
    ProcessMemory memory;
    std::map<std::string, std::vector<std::uint8_t>> files;
    Modules modules{memory,
                    [&](const std::string& name) {
                        if (!files.contains(name))
                            throw PeError("missing dependency");
                        return files.at(name);
                    },
                    {}};
};
}  // namespace
TEST_CASE("WinCE modules: exports, ordinals, dependency cycles and automatic rebasing") {
    Harness h;
    h.files["a.dll"] = module(true, "B.DLL");
    h.files["b.dll"] = module(true, "A.DLL");
    const auto root = h.modules.load("test.exe", module(false, "a.dll"));
    CHECK(root == 0x10000);
    const auto a = h.modules.handle("A"), b = h.modules.handle("b.dll");
    CHECK(a == 0x01000000);
    CHECK(b == 0x01010000);
    CHECK(h.memory.read32(root + 0x1260) == a + 0x1000);
    CHECK(h.memory.read32(a + 0x1260) == b + 0x1000);
    CHECK(h.memory.read32(b + 0x1260) == a + 0x1000);
    CHECK(h.memory.read32(b + 0x1010) == b + 0x1000);
    Import ordinal;
    ordinal.ordinal = 1;
    CHECK(h.modules.procedure(b, ordinal) == b + 0x1000);
    CHECK(h.modules.procedure(a, Import{"", "Work"}) == a + 0x1000);
    CHECK(h.modules.procedure(a, Import{"", "work"}) == 0);
    std::vector<std::uint32_t> initialized;
    h.modules.initialize([&](auto entry, auto instance, auto reason) {
        CHECK(entry == instance + 0x1000);
        CHECK(reason == 1);
        initialized.push_back(instance);
        return true;
    });
    REQUIRE(initialized.size() == 2);
    CHECK(initialized[0] == b);
    CHECK(initialized[1] == a);
    CHECK(h.modules.load_library("A.DLL") == a);
    h.modules.initialize({});
    CHECK(initialized.size() == 2);
}
TEST_CASE("WinCE modules: forwarded name and ordinal exports") {
    Harness h;
    h.files["a.dll"] = module(true, "", "B.#1");
    h.files["b.dll"] = module();
    SUBCASE("named forwarding") {
        h.files["a.dll"] = module(true, "", "B.Work");
    }
    h.modules.load("test.exe", module(false, "A.DLL"));
    CHECK(h.memory.read32(0x11260) == h.modules.handle("b.dll") + 0x1000);
    CHECK(h.modules.procedure(h.modules.handle("a.dll"), Import{"", "Work"}) ==
          h.modules.handle("b.dll") + 0x1000);
}
TEST_CASE("WinCE modules: failed dependency graph does not install partial modules") {
    Harness h;
    h.files["a.dll"] = module(true, "missing.dll");
    SUBCASE("missing symbol") {
        h.files["a.dll"] = module();
        text(h.files["a.dll"], 0x670, "Other");
    }
    SUBCASE("forwarder cycle") {
        h.files["a.dll"] = module(true, "", "A.Work");
    }
    SUBCASE("forwarder malformed ordinal") {
        h.files["a.dll"] = module(true, "", "B.#65536");
        h.files["b.dll"] = module();
    }
    SUBCASE("dependency is EXE") {
        h.files["a.dll"] = module(false);
    }
    CHECK_THROWS_AS(h.modules.load("test.exe", module(false, "a.dll")), PeError);
    CHECK(h.modules.all().empty());
    CHECK(h.modules.handle("test.exe") == 0);
    CHECK_THROWS_AS(h.memory.read32(0x10000), std::runtime_error);
    CHECK_THROWS_AS(h.modules.load_library("../a.dll"), PeError);
}
TEST_CASE("WinCE modules: malformed export tables and data exports") {
    auto bytes = module();
    SUBCASE("export aliases share an ordinal") {
        p32(bytes, 0x618, 2);
        p32(bytes, 0x654, 0x1490);
        p16(bytes, 0x662, 0);
        text(bytes, 0x690, "Alias");
        const auto im = prepare_pe(bytes);
        REQUIRE(im.exports.size() == 2);
        CHECK(im.exports[0].ordinal == im.exports[1].ordinal);
        CHECK(im.exports[0].rva == im.exports[1].rva);
        CHECK(im.exports[1].name == "Alias");
        text(bytes, 0x690, "Work");
        CHECK_THROWS_AS(prepare_pe(bytes), PeError);
    }
    SUBCASE("bad ordinal") {
        p16(bytes, 0x660, 1);
        CHECK_THROWS_AS(prepare_pe(bytes), PeError);
    }
    SUBCASE("unmapped export") {
        p32(bytes, 0x640, 0x800);
        CHECK_THROWS_AS(prepare_pe(bytes), PeError);
    }
    SUBCASE("empty export") {
        p32(bytes, 0x640, 0);
        CHECK_THROWS_AS(prepare_pe(bytes), PeError);
    }
    SUBCASE("truncated directory") {
        p32(bytes, 0xFC, 12);
        CHECK_THROWS_AS(prepare_pe(bytes), PeError);
    }
    SUBCASE("odd data export") {
        p32(bytes, 0x640, 0x1011);
        Harness h;
        h.files["a.dll"] = bytes;
        h.modules.load("test.exe", module(false, "a.dll"));
        CHECK(h.memory.read32(0x11260) == h.modules.handle("a.dll") + 0x1011);
    }
}
TEST_CASE("WinCE modules: failed DLL attach poisons further use") {
    Harness h;
    h.files["a.dll"] = module();
    h.modules.load_library("a.dll");
    CHECK_THROWS_AS(h.modules.initialize([](auto, auto, auto) { return false; }), PeError);
    CHECK_THROWS_AS(h.modules.load_library("a.dll"), PeError);
    CHECK_THROWS_AS(h.modules.procedure(0x10000, Import{"", "Work"}), PeError);
}
TEST_CASE("WinCE kernel: dynamic module API, wide symbols and named event lifetime") {
    Harness h;
    Kernel k;
    h.files["a.dll"] = module();
    unsigned attaches = 0;
    k.attach_modules(h.modules, 0x20000, [&](auto, auto, auto) {
        ++attaches;
        return true;
    });
    dream::sh4::Ctx cpu{};
    const auto strings = h.memory.allocate(4096);
    const auto wide = [&](std::uint32_t at, std::u16string_view s) {
        for (unsigned i = 0; i < s.size(); ++i) h.memory.write16(at + i * 2, s[i]);
        h.memory.write16(at + static_cast<std::uint32_t>(s.size()) * 2, 0);
    };
    const auto call = [&](const char* name, std::uint32_t a = 0, std::uint32_t b = 0,
                          std::uint32_t c = 0, std::uint32_t d = 0) {
        cpu.r[4] = a;
        cpu.r[5] = b;
        cpu.r[6] = c;
        cpu.r[7] = d;
        const auto trap = k.resolve(Import{"coredll.dll", name});
        REQUIRE(trap.has_value());
        const bool invoked = k.invoke(cpu, h.memory, *trap);
        REQUIRE(invoked);
        return cpu.r[0];
    };
    wide(strings, u"A.DLL");
    wide(strings + 32, u"Work");
    const auto a = call("LoadLibraryW", strings);
    CHECK(a == 0x10000);
    CHECK(attaches == 1);
    CHECK(call("LoadLibrary", strings) == a);
    CHECK(attaches == 1);
    CHECK(call("GetModuleHandleW", strings) == a);
    CHECK(call("GetModuleHandleW", 0) == 0x20000);
    CHECK(call("GetProcAddressW", a, strings + 32) == a + 0x1000);
    CHECK(call("GetProcAddress", a, 1) == a + 0x1000);
    CHECK(call("GetProcAddressW", a, 2) == 0);
    CHECK(k.last_error() == 127);
    CHECK(call("LoadLibraryExW", strings, 0, 2) == 0);
    CHECK(k.last_error() == 120);
    wide(strings, u"event");
    const auto e1 = call("CreateEventW", 0, 0, 1, strings);
    const auto e2 = call("CreateEventW", 0, 1, 1, strings);
    CHECK(e1 != e2);
    CHECK(k.last_error() == 183);
    CHECK(call("WaitForSingleObject", e1, 0) == 0);
    CHECK(call("WaitForSingleObject", e2, 5) ==
          258);  // Existing auto-reset event keeps original attributes.
    CHECK(cpu.cycles == 1000000);
    CHECK(call("SetEvent", e2) == 1);
    CHECK(call("WaitForSingleObject", e1, 0) == 0);
    CHECK_THROWS_AS(call("WaitForSingleObject", e1, 0xFFFFFFFF), std::runtime_error);
    CHECK(call("CloseHandle", e1) == 1);
    CHECK(call("SetEvent", e1) == 0);
    CHECK(k.last_error() == 6);
    CHECK(call("CloseHandle", e2) == 1);
    const auto e3 = call("CreateEventW", 0, 1, 1, strings);
    CHECK(k.last_error() == 0);
    CHECK(call("WaitForSingleObject", e3, 0) == 0);
    CHECK(call("WaitForSingleObject", e3, 0) == 0);
    CHECK(call("ResetEvent", e3) == 1);
    CHECK(call("WaitForSingleObject", e3, 0) == 258);
}
#ifdef DREAM_DEV_INTERPRETER
TEST_CASE("WinCE modules: SH4 executes DLL initialization and calls linked guest export") {
    Harness h;
    h.files["a.dll"] = module();
    auto app = module(false, "a.dll");
    const std::uint16_t code[]{0x4F22, 0x7FF0, 0xD206, 0x6222, 0x420B,
                               0x0009, 0x7F10, 0x4F26, 0x000B, 0x0009};
    for (unsigned i = 0; i < std::size(code); ++i) p16(app, 0x200 + i * 2, code[i]);
    p32(app, 0x220, 0x11260);
    p16(app, 0x708, 0x3020);
    h.modules.load("test.exe", app);
    dream::sh4::Ctx cpu{};
    cpu.r[15] = h.memory.allocate(65536) + 65536;
    cpu.next_event = ~0ull;
    dream::devinterp::Options options;
    options.call_translated = false;
    options.max_instructions = 100;
    h.modules.initialize([&](auto entry, auto instance, auto reason) {
        cpu.pc = entry;
        cpu.pr = 0x1EFFFFFC;
        cpu.r[4] = instance;
        cpu.r[5] = reason;
        cpu.r[6] = 0;
        CHECK(dream::devinterp::run(cpu, h.memory, cpu.pr, options) ==
              dream::devinterp::Stop::Returned);
        return cpu.r[0] != 0;
    });
    cpu.pc = 0x11000;
    cpu.pr = 0x1EFFFFFC;
    dream::devinterp::Stats stats;
    CHECK(dream::devinterp::run(cpu, h.memory, cpu.pr, options, &stats) ==
          dream::devinterp::Stop::Returned);
    CHECK(cpu.r[0] == 1);
    CHECK(stats.instructions == 13);
    CHECK(stats.native_calls == 0);
}
#endif
