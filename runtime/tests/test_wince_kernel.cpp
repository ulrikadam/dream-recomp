#include "dream/runtime/wince/abi.h"
#include "dream/runtime/wince/heap.h"

#include "doctest.h"
#include "wince_fixture.h"
#ifdef DREAM_DEV_INTERPRETER
#include "dream/runtime/devinterp/interpreter.h"
#endif

using namespace dream::wince;
namespace {
struct Guest {
    ProcessMemory m;
    Kernel kernel;
    dream::sh4::Ctx cpu{};
    std::uint32_t data = m.allocate(16384);
    Guest() {
        cpu.r[15] = data + 16000;
        cpu.next_event = ~0ull;
        for (unsigned i = 0; i < 4; ++i) m.write32(cpu.r[15] + i * 4, 0xBAD0BAD0 + i);
    }
    std::uint32_t p(unsigned offset = 0) const { return data + offset; }
    std::uint32_t call(const char* name, std::initializer_list<std::uint32_t> args = {}) {
        unsigned i = 0;
        for (const auto value : args) {
            if (i < 4)
                cpu.r[4 + i] = value;
            else
                m.write32(cpu.r[15] + i * 4, value);
            ++i;
        }
        const auto trap = kernel.resolve(Import{"COREDLL.DLL", name});
        REQUIRE(trap.has_value());
        const bool called = kernel.invoke(cpu, m, *trap);
        REQUIRE(called);
        return cpu.r[0];
    }
    void wide(unsigned offset, std::u16string_view text) {
        for (unsigned i = 0; i < text.size(); ++i) m.write16(p(offset + i * 2), text[i]);
        m.write16(p(offset + static_cast<unsigned>(text.size()) * 2), 0);
    }
    void bytes(unsigned offset, std::string_view text) {
        for (unsigned i = 0; i < text.size(); ++i)
            m.write8(p(offset + i), static_cast<std::uint8_t>(text[i]));
        m.write8(p(offset + static_cast<unsigned>(text.size())), 0);
    }
    std::u16string wide_at(std::uint32_t p) {
        std::u16string s;
        for (unsigned i = 0; i < 1024; ++i) {
            const auto c = m.read16(p + i * 2);
            if (!c)
                return s;
            s += static_cast<char16_t>(c);
        }
        throw std::runtime_error("missing test terminator");
    }
};
}  // namespace
TEST_CASE("WinCE compiler helpers: signed division boundaries and quotient/remainder identities") {
    Guest g;
    g.call("SetLastError", {0x55});
    const std::array<std::array<std::uint32_t, 4>, 8> signed_cases{
        {{7, 3, 2, 1},
         {0xFFFFFFF9u, 3, 0xFFFFFFFEu, 0xFFFFFFFFu},
         {7, 0xFFFFFFFDu, 0xFFFFFFFEu, 1},
         {0xFFFFFFF9u, 0xFFFFFFFDu, 2, 0xFFFFFFFFu},
         {0x80000000u, 0xFFFFFFFFu, 0x80000000u, 0},
         {0x80000000u, 3, 0xD5555556u, 0xFFFFFFFEu},
         {0x7FFFFFFFu, 2, 0x3FFFFFFFu, 1},
         {0, 0x80000000u, 0, 0}}};
    for (const auto& row : signed_cases) {
        CHECK(g.call("_divls", {row[0], row[1]}) == row[2]);
        CHECK(g.call("_modls", {row[0], row[1]}) == row[3]);
    }
    CHECK(g.call("_divlu", {0xFFFFFFFFu, 0x80000000u}) == 1);
    CHECK(g.call("_modlu", {0xFFFFFFFFu, 0x80000000u}) == 0x7FFFFFFFu);
    CHECK(g.call("_divlu", {0xFFFFFFFFu, 1}) == 0xFFFFFFFFu);
    const auto signed_value = [](std::uint32_t x) {
        return x <= 0x7FFFFFFFu ? std::int64_t{x} : -std::int64_t{(~x) + 1u};
    };
    const auto magnitude = [](std::int64_t x) { return x < 0 ? -x : x; };
    std::uint32_t seed = 0x31415926;
    const auto next = [&]() { return seed = seed * 1664525u + 1013904223u; };
    for (unsigned i = 0; i < 1024; ++i) {
        const auto a = next(), b = next() | 1u;
        const auto uq = g.call("_divlu", {a, b}), ur = g.call("_modlu", {a, b});
        CHECK(std::uint64_t{uq} * b + ur == a);
        CHECK(ur < b);
        const auto n = signed_value(a), d = signed_value(b);
        const auto q = signed_value(g.call("_divls", {a, b}));
        const auto r = signed_value(g.call("_modls", {a, b}));
        CHECK(q * d + r == n);
        CHECK(magnitude(r) < magnitude(d));
        CHECK((r == 0 || ((r < 0) == (n < 0))));
    }
    CHECK(g.kernel.last_error() == 0x55);
}

TEST_CASE("WinCE compiler helpers: zero divisors fault without changing results or last-error") {
    Guest g;
    for (const auto* name : {"_divls", "_divlu", "_modls", "_modlu"}) {
        g.call("SetLastError", {42});
        g.cpu.r[0] = 0xDEADBEEF;
        CHECK_THROWS_WITH(g.call(name, {7, 0}),
                          "SH-4 division by zero requires WinCE exception dispatch");
        CHECK(g.cpu.r[0] == 0xDEADBEEF);
        CHECK(g.kernel.last_error() == 42);
    }
}

TEST_CASE("WinCE heaps: suballocation, reuse, coalescing and API ownership") {
    Guest g;
    const auto heap = g.call("GetProcessHeap");
    REQUIRE(heap != 0);
    std::vector<std::uint32_t> blocks;
    for (unsigned i = 0; i < 2048; ++i) {
        const auto p = g.call("HeapAlloc", {heap, 8, 8});
        REQUIRE(p != 0);
        CHECK((p & 7) == 0);
        CHECK(g.m.read64(p) == 0);
        blocks.push_back(p);
    }
    CHECK(blocks.back() - blocks.front() < 65536);  // Small objects do not each consume a page.
    for (unsigned i = 0; i < blocks.size(); i += 2)
        CHECK(g.call("HeapFree", {heap, 0, blocks[i]}) == 1);
    CHECK(g.call("HeapAlloc", {heap, 0, 8}) == blocks[0]);
    CHECK(g.call("HeapValidate", {heap, 0, blocks[1]}) == 1);
    CHECK(g.call("HeapValidate", {heap, 0, blocks[2]}) == 0);
    CHECK(g.call("HeapFree", {heap, 0, blocks[1] + 1}) == 0);
    CHECK(g.call("HeapFree", {heap, 0, g.data}) == 0);
    CHECK(g.m.read32(g.data) == 0);  // Stack/scratch allocations survive invalid frees.
    CHECK(g.call("LocalFree", {blocks[1]}) == blocks[1]);
    CHECK(g.kernel.last_error() == 6);
    CHECK(g.call("HeapDestroy", {heap}) == 0);
    CHECK(g.kernel.last_error() == 87);
    CHECK(g.call("HeapAlloc", {heap + 1, 0, 8}) == 0);
    CHECK(g.kernel.last_error() == 6);
}
TEST_CASE("WinCE heaps: private bounds, reallocation preservation and destruction") {
    Guest g;
    const auto heap = g.call("HeapCreate", {0, 4096, 4096});
    REQUIRE(heap != 0);
    const auto a = g.call("HeapAlloc", {heap, 0, 16}), b = g.call("HeapAlloc", {heap, 0, 16});
    REQUIRE(a != 0);
    REQUIRE(b != 0);
    g.m.write32(a, 0x12345678);
    CHECK(g.call("HeapReAlloc", {heap, 0x18, a, 64}) == 0);
    CHECK(g.m.read32(a) == 0x12345678);
    const auto moved = g.call("HeapReAlloc", {heap, 8, a, 64});
    REQUIRE(moved != 0);
    CHECK(moved != a);
    CHECK(g.m.read32(moved) == 0x12345678);
    CHECK(g.m.read32(moved + 60) == 0);
    CHECK(g.call("HeapSize", {heap, 0, a}) == 0xFFFFFFFF);
    CHECK(g.call("HeapSize", {heap, 0, moved}) == 64);
    CHECK(g.call("HeapReAlloc", {heap, 0, moved, 8192}) == 0);
    CHECK(g.m.read32(moved) == 0x12345678);
    CHECK(g.call("HeapReAlloc", {heap, 0x10, moved, 8}) == moved);
    CHECK(g.call("HeapSize", {heap, 0, moved}) == 8);
    CHECK(g.call("HeapDestroy", {heap}) == 1);
    CHECK(g.call("HeapFree", {heap, 0, b}) == 0);
    CHECK_THROWS_AS(g.m.read8(moved), std::runtime_error);
    CHECK(g.call("HeapCreate", {0, 8192, 4096}) == 0);
    CHECK(g.call("HeapCreate", {4, 0, 0}) == 0);
    CHECK(g.kernel.last_error() == 120);
}
TEST_CASE("WinCE local and CRT allocators: growth zeroing, overflow and mismatched frees") {
    Guest g;
    const auto a = g.call("LocalAlloc", {0x40, 16}), b = g.call("LocalAlloc", {0, 16});
    REQUIRE(a != 0);
    REQUIRE(b != 0);
    g.m.write32(a, 0xCAFE);
    CHECK(g.call("LocalReAlloc", {a, 100, 0}) == 0);
    CHECK(g.call("LocalSize", {a}) == 16);
    const auto moved = g.call("LocalReAlloc", {a, 100, 0x42});
    REQUIRE(moved != 0);
    CHECK(g.m.read32(moved) == 0xCAFE);
    CHECK(g.m.read32(moved + 96) == 0);
    CHECK(g.call("LocalSize", {moved}) == 100);
    CHECK(g.call("LocalFree", {moved}) == 0);
    CHECK(g.call("LocalFree", {moved}) == moved);
    CHECK(g.call("calloc", {0x80000000, 4}) == 0);
    const auto c = g.call("calloc", {10, 4});
    REQUIRE(c != 0);
    CHECK(g.m.read32(c + 36) == 0);
    g.m.write32(c, 123);
    const auto r = g.call("realloc", {c, 100});
    REQUIRE(r != 0);
    CHECK(g.m.read32(r) == 123);
    CHECK(g.call("realloc", {r, 0xFFFFFFFF}) == 0);
    CHECK(g.m.read32(r) == 123);
    CHECK(g.call("LocalFree", {r}) == r);
    CHECK(g.call("realloc", {r, 0}) == 0);
    CHECK_THROWS_AS(g.call("free", {r}), MemoryError);
    CHECK(g.call("free", {0}) == 0);
}
TEST_CASE("WinCE heaps: deterministic fragmentation trace preserves all live blocks") {
    Guest g;
    const auto heap = g.call("HeapCreate", {0, 0, 65536});
    REQUIRE(heap != 0);
    std::map<std::uint32_t, std::vector<std::uint8_t>> live;
    std::uint32_t rng = 0x5A17;
    auto random = [&]() {
        rng = rng * 1664525u + 1013904223u;
        return rng;
    };
    for (unsigned step = 0; step < 1200; ++step) {
        const auto operation = random() % 3;
        if (live.empty() || operation == 0) {
            const auto size = 1 + random() % 768;
            const auto p = g.call("HeapAlloc", {heap, 0, size});
            if (p) {
                REQUIRE_FALSE(live.contains(p));
                live.emplace(p, std::vector<std::uint8_t>(size, static_cast<std::uint8_t>(step)));
                g.m.write_bytes(p, live.at(p));
            }
        } else {
            auto it = std::next(live.begin(), random() % live.size());
            const auto p = it->first;
            if (operation == 1) {
                CHECK(g.call("HeapFree", {heap, 0, p}) == 1);
                live.erase(it);
            } else {
                const auto size = 1 + random() % 1536;
                const auto replacement = g.call("HeapReAlloc", {heap, 0, p, size});
                if (replacement) {
                    const auto count = std::min<std::size_t>(size, it->second.size());
                    CHECK(
                        g.m.read_bytes(replacement, static_cast<std::uint32_t>(count)) ==
                        std::vector<std::uint8_t>(it->second.begin(), it->second.begin() + count));
                    live.erase(it);
                    live.emplace(replacement,
                                 std::vector<std::uint8_t>(size, static_cast<std::uint8_t>(step)));
                    g.m.write_bytes(replacement, live.at(replacement));
                }
            }
        }
        for (const auto& [p, contents] : live)
            CHECK(g.m.read_bytes(p, static_cast<std::uint32_t>(contents.size())) == contents);
    }
    CHECK(g.call("HeapDestroy", {heap}) == 1);
}
TEST_CASE("WinCE virtual memory: reserve, subrange commit, query and zero after decommit") {
    Guest g;
    const auto base = g.call("VirtualAlloc", {0, 12288, 0x2000, 4});
    REQUIRE(base == 0x20000000);
    CHECK_THROWS_AS(g.m.read8(base), std::runtime_error);
    REQUIRE(g.call("VirtualQuery", {base + 10, g.p(), 28}) == 28);
    CHECK(g.m.read32(g.p(0)) == base);
    CHECK(g.m.read32(g.p(4)) == base);
    CHECK(g.m.read32(g.p(12)) == 12288);
    CHECK(g.m.read32(g.p(16)) == 0x2000);
    CHECK(g.call("VirtualAlloc", {base + 4095, 2, 0x1000, 4}) == base);
    g.m.write32(base, 123);
    g.m.write32(base + 4096, 456);
    CHECK(g.call("VirtualAlloc", {base, 4096, 0x1000, 4}) == base);
    CHECK(g.m.read32(base) == 123);
    CHECK(g.call("VirtualFree", {base, 4096, 0x4000}) == 1);
    CHECK_THROWS_AS(g.m.read8(base), std::runtime_error);
    CHECK(g.m.read32(base + 4096) == 456);
    CHECK(g.call("VirtualAlloc", {base, 4096, 0x1000, 4}) == base);
    CHECK(g.m.read32(base) == 0);
    CHECK(g.call("VirtualQuery", {base + 4096, g.p(), 28}) == 28);
    CHECK(g.m.read32(g.p(12)) == 4096);
    CHECK(g.call("VirtualAlloc", {base + 1, 1, 0x2000, 4}) == 0);
    CHECK(g.kernel.last_error() == 487);
    CHECK(g.call("VirtualFree", {base + 4096, 0, 0x8000}) == 0);
    CHECK(g.call("VirtualFree", {base, 1, 0x8000}) == 0);
    CHECK(g.call("VirtualFree", {base, 0, 0x8000}) == 1);
    CHECK(g.call("VirtualQuery", {base, g.p(), 28}) == 28);
    CHECK(g.m.read32(g.p(16)) == 0x10000);
    CHECK(g.call("VirtualAlloc", {0, 4096, 0x3000, 4}) == base);
}
TEST_CASE("WinCE virtual memory: atomic protections, executable pages and pointer checks") {
    Guest g;
    const auto base = g.call("VirtualAlloc", {0, 8192, 0x3000, 4});
    REQUIRE(base != 0);
    g.m.write32(base + 4092, 0x11223344);
    g.m.write32(base + 4096, 0x55667788);
    CHECK(g.call("VirtualProtect", {base + 4096, 4096, 2, g.p()}) == 1);
    CHECK(g.m.read32(g.p()) == 4);
    CHECK_THROWS_AS(g.m.write64(base + 4092, 0), std::runtime_error);
    CHECK(g.m.read32(base + 4092) == 0x11223344);
    CHECK(g.m.read32(base + 4096) == 0x55667788);
    CHECK(g.call("VirtualProtect", {base, 4096, 2, 0}) == 0);
    g.m.write32(base, 123);
    CHECK(g.call("VirtualProtect", {base, 12288, 2, g.p()}) == 0);
    g.m.write32(base, 456);
    CHECK(g.call("VirtualProtect", {base, 4096, 0x104, g.p()}) == 0);
    CHECK(g.kernel.last_error() == 87);
    g.m.write16(base, 0xE007);
    g.m.write16(base + 2, 0x000B);
    g.m.write16(base + 4, 0x0009);
    CHECK(g.call("VirtualProtect", {base, 4096, 0x20, g.p()}) == 1);
    CHECK(g.m.fetch16(base) == 0xE007);
    CHECK_THROWS_AS(g.m.write16(base, 0), std::runtime_error);
#ifdef DREAM_DEV_INTERPRETER
    g.cpu.pc = base;
    g.cpu.pr = 0x1EFFFFFC;
    dream::devinterp::Options options;
    options.call_translated = false;
    options.max_instructions = 8;
    CHECK(dream::devinterp::run(g.cpu, g.m, g.cpu.pr, options) == dream::devinterp::Stop::Returned);
    CHECK(g.cpu.r[0] == 7);
#endif
    CHECK(g.call("VirtualProtect", {base, 4096, 0x10, g.p()}) == 1);
    CHECK(g.m.fetch16(base) == 0xE007);
    CHECK_THROWS_AS(g.m.read16(base), std::runtime_error);
    CHECK(g.call("VirtualProtect", {base + 4096, 4096, 4, g.p()}) == 1);
    CHECK(g.call("VirtualProtect", {base + 4096, 4096, 2, base + 4096}) == 1);
    CHECK(g.m.read32(base + 4096) == 4);  // Output in the newly read-only range is written first.
}
TEST_CASE("WinCE virtual memory: failed commit and image collision preserve reservations") {
    Guest g;
    const auto base = g.call("VirtualAlloc", {0, ProcessMemory::kBudget, 0x2000, 4});
    REQUIRE(base != 0);
    CHECK(g.call("VirtualAlloc", {base, ProcessMemory::kBudget, 0x1000, 4}) == 0);
    CHECK(g.kernel.last_error() == 8);
    CHECK(g.m.virtual_query(base)[4] == 0x2000);
    CHECK(g.m.virtual_query(base + ProcessMemory::kBudget - 4096)[4] == 0x2000);
    Image image;
    image.base = base;
    image.bytes.resize(4096);
    image.headers_size = 128;
    CHECK_THROWS_AS(g.m.install(image), std::runtime_error);
    CHECK(g.call("VirtualFree", {base, 0, 0x8000}) == 1);
    const auto top = g.call("VirtualAlloc", {0, 4096, 0x103000, 4});
    CHECK(top == 0x3FFF0000);
    CHECK(g.call("VirtualAlloc", {0xFFFFFFF0, 4096, 0x3000, 4}) == 0);
    CHECK(g.call("VirtualAlloc", {0, 0, 0x3000, 4}) == 0);
}
TEST_CASE("WinCE virtual memory: many committed pages do not exhaust the heap handle budget") {
    Guest g;
    const auto base = g.call("VirtualAlloc", {0, 16u << 20, 0x3000, 4});
    REQUIRE(base != 0);
    const auto block = g.call("LocalAlloc", {0x40, 64});
    REQUIRE(block != 0);
    const auto bytes = g.m.read_bytes(base + 4090, 12300);
    CHECK(std::all_of(bytes.begin(), bytes.end(), [](auto b) { return b == 0; }));
    g.m.write_bytes(base + 4090, std::vector<std::uint8_t>(12300, 0x5A));
    CHECK(g.m.read8(base + 4090 + 12299) == 0x5A);
    CHECK(g.call("LocalFree", {block}) == 0);
    CHECK(g.call("VirtualFree", {base, 0, 0x8000}) == 1);
}
TEST_CASE("WinCE CRT: overlapping copies, atomic invalid writes and unsigned comparisons") {
    Guest g;
    g.bytes(0, "abcdef");
    CHECK(g.call("memmove", {g.p(1), g.p(), 5}) == g.p(1));
    CHECK(g.m.read_bytes(g.p(), 7) == std::vector<std::uint8_t>{'a', 'a', 'b', 'c', 'd', 'e', 0});
    CHECK(g.call("memset", {g.p(16), 0x180, 3}) == g.p(16));
    CHECK(g.m.read8(g.p(16)) == 128);
    g.m.write8(g.p(24), 127);
    CHECK(static_cast<std::int32_t>(g.call("memcmp", {g.p(16), g.p(24), 1})) > 0);
    CHECK(g.call("memcpy", {0, 0, 0}) == 0);
    CHECK(g.call("memcmp", {0, 0, 0}) == 0);
    const auto last = g.m.allocate(4096);
    g.m.write32(last + 4092, 0xDEADBEEF);
    CHECK_THROWS_AS(g.call("memcpy", {last + 4092, g.p(), 8}), std::runtime_error);
    CHECK(g.m.read32(last + 4092) == 0xDEADBEEF);
    CHECK_THROWS_AS(g.call("memset", {0xFFFFFFFC, 0, 16}), std::runtime_error);
}
TEST_CASE("WinCE CRT: byte and UTF16 strings, truncation, padding, search and ASCII folding") {
    Guest g;
    g.bytes(0, "alpha");
    g.bytes(32, "bet");
    CHECK(g.call("strlen", {g.p()}) == 5);
    CHECK(g.call("strcpy", {g.p(128), g.p()}) == g.p(128));
    CHECK(g.call("strcat", {g.p(128), g.p(32)}) == g.p(128));
    CHECK(g.call("strlen", {g.p(128)}) == 8);
    CHECK(g.call("strncpy", {g.p(256), g.p(), 3}) == g.p(256));
    CHECK(g.m.read_bytes(g.p(256), 3) == std::vector<std::uint8_t>{'a', 'l', 'p'});
    g.call("memset", {g.p(256), 0xFF, 16});
    g.call("strncpy", {g.p(256), g.p(), 12});
    CHECK(g.m.read32(g.p(264)) == 0);
    CHECK(g.call("strchr", {g.p(), 'p'}) == g.p(2));
    CHECK(g.call("strrchr", {g.p(), 'a'}) == g.p(4));
    CHECK(g.call("strrchr", {g.p(), 0}) == g.p(5));
    CHECK(g.call("strchr", {g.p(), 'z'}) == 0);
    CHECK(g.call("strncmp", {0, 0, 0}) == 0);
    CHECK(static_cast<std::int32_t>(g.call("strcmp", {g.p(), g.p(32)})) < 0);
    g.wide(512, u"A\u03A9\U0001F600");
    g.wide(640, u"abc");
    g.wide(704, u"ABC");
    CHECK(g.call("wcslen", {g.p(512)}) == 4);
    CHECK(g.call("wcscpy", {g.p(1024), g.p(512)}) == g.p(1024));
    CHECK(g.wide_at(g.p(1024)) == u"A\u03A9\U0001F600");
    CHECK(g.call("wcsrchr", {g.p(512), 0x03A9}) == g.p(514));
    CHECK(g.call("_wcsicmp", {g.p(640), g.p(704)}) == 0);
    CHECK(g.call("_wcsnicmp", {g.p(640), g.p(704), 2}) == 0);
    CHECK(g.call("lstrcmpiW", {g.p(640), g.p(704)}) == 0);
    CHECK_THROWS_AS(g.call("lstrcmpiW", {g.p(512), g.p(512)}), MemoryError);
    CHECK(g.call("CharPrevW", {g.p(512), g.p(516)}) == g.p(514));
    CHECK(g.call("CharPrevW", {g.p(512), g.p(512)}) == g.p(512));
}
TEST_CASE("WinCE kernel: interlocked operations, 64-bit virtual time and captured diagnostics") {
    Guest g;
    g.m.write32(g.p(), 0x7FFFFFFF);
    CHECK(g.call("InterlockedIncrement", {g.p()}) == 0x80000000);
    CHECK(g.call("InterlockedDecrement", {g.p()}) == 0x7FFFFFFF);
    CHECK(g.call("InterlockedExchange", {g.p(), 5}) == 0x7FFFFFFF);
    CHECK(g.m.read32(g.p()) == 5);
    CHECK(g.call("InterlockedCompareExchange", {g.p(), 8, 5}) == 5);
    CHECK(g.m.read32(g.p()) == 8);
    CHECK(g.call("InterlockedCompareExchange", {g.p(), 9, 5}) == 8);
    CHECK(g.m.read32(g.p()) == 8);
    CHECK(g.call("InterlockedTestExchange", {g.p(), 8, 11}) == 8);
    CHECK(g.m.read32(g.p()) == 11);
    CHECK_THROWS_AS(g.call("InterlockedIncrement", {g.p(1)}), std::runtime_error);
    g.cpu.cycles = 0x123456789ABCull;
    CHECK(g.call("QueryPerformanceCounter", {g.p()}) == 1);
    CHECK(g.m.read64(g.p()) == g.cpu.cycles);
    CHECK(g.call("QueryPerformanceFrequency", {g.p()}) == 1);
    CHECK(g.m.read64(g.p()) == 200000000);
    CHECK(g.call("QueryPerformanceCounter", {0}) == 0);
    CHECK(g.kernel.last_error() == 998);
    g.wide(128, u"startup \u03A9");
    g.call("OutputDebugStringW", {g.p(128)});
    REQUIRE(g.kernel.debug_messages().size() == 1);
    CHECK(g.kernel.debug_messages()[0] == u"startup \u03A9");
}
TEST_CASE("WinCE formatter: register varargs, home-space gap, precision and atomic failures") {
    Guest g;
    g.wide(256, u"%s %08X %d %u");
    g.wide(512, u"CE");
    const auto count =
        g.call("wsprintfW", {g.p(), g.p(256), g.p(512), 0xBEEF, 0x80000000, 0xFFFFFFFF});
    CHECK(g.wide_at(g.p()) == u"CE 0000BEEF -2147483648 4294967295");
    CHECK(count == 34);
    CHECK(g.m.read32(g.cpu.r[15]) == 0xBAD0BAD0);
    CHECK(g.m.read32(g.cpu.r[15] + 12) == 0xBAD0BAD3);
    g.wide(256, u"%-5.2s/%05d/%%/%S");
    g.bytes(640, "ok");
    CHECK(g.call("wsprintfW",
                 {g.p(), g.p(256), g.p(512), static_cast<std::uint32_t>(-42), g.p(640)}) == 16);
    CHECK(g.wide_at(g.p()) == u"CE   /-0042/%/ok");
    g.wide(256, u"%d:%d:%d");
    g.m.write32(g.p(768), 1);
    g.m.write32(g.p(772), 2);
    g.m.write32(g.p(776), 3);
    CHECK(g.call("wvsprintfW", {g.p(), g.p(256), g.p(768)}) == 5);
    CHECK(g.wide_at(g.p()) == u"1:2:3");
    g.wide(256, u"%f");
    CHECK(g.call("wsprintfW", {g.p(), g.p(256), 0}) == 0);
    CHECK(g.kernel.last_error() == 120);
    CHECK(g.wide_at(g.p()) == u"1:2:3");
    g.wide(256, u"%9999999s");
    CHECK(g.call("wsprintfW", {g.p(), g.p(256), g.p(512)}) == 0);
    CHECK(g.kernel.last_error() == 122);
    CHECK(g.wide_at(g.p()) == u"1:2:3");
    g.wide(256, u"%511d%%");
    CHECK(g.call("wsprintfW", {g.p(), g.p(256), 1}) == 0);
    CHECK(g.kernel.last_error() == 122);
    CHECK(g.wide_at(g.p()) == u"1:2:3");
}
#ifdef DREAM_DEV_INTERPRETER
TEST_CASE("WinCE kernel execution: PE wsprintf uses stack argument five at SP plus 16") {
    Guest g;
    auto file = wince_test::module(false, "COREDLL.DLL");
    wince_test::text(file, 0x4C2, "wsprintfW");
    const std::uint16_t code[]{0x4F22, 0x7FEC, 0xE02A, 0x1F04, 0xD205, 0x6222,
                               0x420B, 0x0009, 0x7F14, 0x4F26, 0x000B, 0x0009};
    for (unsigned i = 0; i < std::size(code); ++i) wince_test::p16(file, 0x200 + i * 2, code[i]);
    wince_test::p32(file, 0x220, 0x11260);
    wince_test::p16(file, 0x708, 0x3020);
    const auto im = load_pe(file, 0x20000, [&](const Import& i) { return g.kernel.resolve(i); });
    g.m.install(im);
    g.wide(256, u"%d/%d/%d");
    g.cpu.r[4] = g.p();
    g.cpu.r[5] = g.p(256);
    g.cpu.r[6] = 11;
    g.cpu.r[7] = 22;
    g.cpu.pc = im.entry;
    g.cpu.pr = 0x1EFFFFFC;
    dream::devinterp::Options options;
    options.call_translated = false;
    options.max_instructions = 64;
    options.hle_call = [&](auto& c, auto& m, auto t) { return g.kernel.invoke(c, m, t); };
    CHECK(dream::devinterp::run(g.cpu, g.m, g.cpu.pr, options) == dream::devinterp::Stop::Returned);
    CHECK(g.cpu.r[0] == 8);
    CHECK(g.wide_at(g.p()) == u"11/22/42");
}
#endif
