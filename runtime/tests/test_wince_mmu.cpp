#include "dream/runtime/mem/mmu_memory.h"

#include "doctest.h"
#ifdef DREAM_DEV_INTERPRETER
#include "dream/runtime/devinterp/interpreter.h"
#endif

using namespace dream;
namespace {
struct MmuFixture {
    mem::DcMemory bus;
    sh4::Ctx cpu{};
    mem::MmuMemory memory{bus, cpu};
    MmuFixture() { bus.write32(0xFF000010u, 1); }
    void map(unsigned index, std::uint32_t va = 0x10000, std::uint32_t pa = 0x0C000000,
             std::uint32_t attributes = 0x174, std::uint8_t asid = 0) {
        bus.utlb().load(index, va | asid, pa | attributes, 0);
    }
};
void expect_fault(MmuFixture& f, std::uint32_t address, std::uint32_t event, bool write = false) {
    try {
        if (write)
            f.memory.write32(address, 0xBAD);
        else
            (void)f.memory.read32(address);
        FAIL("missing MMU fault");
    } catch (const mem::MmuFault& e) {
        CHECK(e.address == address);
        CHECK(e.event == event);
        CHECK(f.bus.read32(0xFF00000Cu) == address);
        CHECK(f.bus.expevt == event);
        if (event != 0x140)
            CHECK((f.bus.read32(0xFF000000u) & 0xFFFFFC00u) == (address & 0xFFFFFC00u));
        CHECK(e.reset == (event == 0x140));
    }
}
}  // namespace

TEST_CASE("WinCE MMU: CPU loads stores and fetches translate while the physical bus stays direct") {
    MmuFixture f;
    f.map(0);
    f.memory.write32(0x10040, 0x12345678);
    CHECK(f.bus.read32(0x8C000040) == 0x12345678);
    CHECK(f.memory.read16(0x10042) == 0x1234);
    CHECK(f.memory.fetch16(0x10040) == 0x5678);
    f.memory.write8(0x10041, 0xAB);
    CHECK(f.memory.read32(0x10040) == 0x1234AB78);
    f.cpu.sr = sh4::SR_MD;
    CHECK(f.memory.read32(0xAC000040) == 0x1234AB78);
    f.bus.write32(0xFF000010, 0);
    CHECK(f.memory.read32(0x0C000040) == 0x1234AB78);
}

TEST_CASE("WinCE MMU: all four SH4 page sizes preserve offset bits") {
    for (unsigned size = 0; size < 4; ++size) {
        MmuFixture f;
        const std::uint32_t sizes[] = {1024, 4096, 65536, 1048576};
        const std::uint32_t bits[] = {0, 0x10, 0x80, 0x90};
        f.map(0, 0x01000000, 0x0C000000, 0x164 | bits[size]);
        const auto last = sizes[size] - 4;
        f.memory.write32(0x01000000 + last, 0xABC000 + size);
        CHECK(f.bus.read32(0x8C000000 + last) == 0xABC000 + size);
        expect_fault(f, 0x01000000 + sizes[size], 0x40);
    }
}

TEST_CASE("WinCE MMU: ASID shared pages SV and duplicate mappings") {
    MmuFixture f;
    f.map(0, 0x10000, 0x0C000000, 0x174, 7);
    f.map(1, 0x10000, 0x0C001000, 0x174, 8);
    f.bus.write32(0xFF000000, 7);
    f.memory.write32(0x10000, 11);
    f.bus.write32(0xFF000000, 8);
    f.memory.write32(0x10000, 22);
    CHECK(f.bus.read32(0x8C000000) == 11);
    CHECK(f.bus.read32(0x8C001000) == 22);
    f.bus.write32(0xFF000000, 9);
    expect_fault(f, 0x10000, 0x40);
    CHECK((f.bus.read32(0xFF000000) & 255) == 9);
    f.cpu.sr = sh4::SR_MD;
    f.bus.write32(0xFF000010, 0x101);  // SV ignores ASID in privileged mode: now two hits
    expect_fault(f, 0x10000, 0x140);
    f.bus.utlb().invalidate();
    f.map(0, 0x10000, 0x0C000000, 0x176, 7);  // shared
    f.cpu.sr = 0;
    CHECK(f.memory.read32(0x10000) == 11);
}

TEST_CASE("WinCE MMU: protection initial-write address faults and invalidation") {
    MmuFixture f;
    for (std::uint32_t pr = 0; pr < 4; ++pr) {
        f.map(0, 0x10000, 0x0C000000, 0x114 | (pr << 5));
        f.cpu.sr = 0;
        if (pr < 2)
            expect_fault(f, 0x10000, 0xA0);
        else
            CHECK_NOTHROW(f.memory.read32(0x10000));
        if (pr != 3)
            expect_fault(f, 0x10000, 0xC0, true);
        else
            CHECK_NOTHROW(f.memory.write32(0x10000, 1));
        f.cpu.sr = sh4::SR_MD;
        CHECK_NOTHROW(f.memory.read32(0x10000));
        if (!(pr & 1))
            expect_fault(f, 0x10000, 0xC0, true);
        else
            CHECK_NOTHROW(f.memory.write32(0x10000, 2));
    }
    f.map(0, 0x10000, 0x0C000000, 0x170);  // dirty bit clear
    expect_fault(f, 0x10000, 0x80, true);
    CHECK(f.bus.read32(0x8C000000) == 2);
    f.cpu.sr = 0;
    expect_fault(f, 0x8C000000, 0xE0);
    expect_fault(f, 0xFF000010, 0x100, true);
    expect_fault(f, 0x10001, 0xE0);
    f.bus.write32(0xFF000010, 5);
    expect_fault(f, 0x10000, 0x40);
    expect_fault(f, 0x10000, 0x60, true);
}

TEST_CASE("WinCE MMU: double store preflights both pages before writing") {
    MmuFixture f;
    f.map(0);
    f.bus.write32(0x8C000FFC, 123);
    CHECK_THROWS_AS(f.memory.write64(0x10FFC, 0xAABBCCDDEEFF0011ull), mem::MmuFault);
    CHECK(f.bus.read32(0x8C000FFC) == 123);
    f.map(1, 0x11000, 0x0C008000);
    f.memory.write64(0x10FFC, 0xAABBCCDDEEFF0011ull);
    CHECK(f.bus.read32(0x8C000FFC) == 0xEEFF0011);
    CHECK(f.bus.read32(0x8C008000) == 0xAABBCCDD);
    CHECK(f.memory.read64(0x10FFC) == 0xAABBCCDDEEFF0011ull);
}

TEST_CASE("WinCE MMU: store queues obey ASIDs and do not fall back on a TLB miss") {
    MmuFixture f;
    f.map(0, 0xE0000000, 0x0C000000, 0x174, 1);
    f.map(1, 0xE0000000, 0x0C001000, 0x174, 2);
    f.bus.write32(0xFF000000, 2);
    for (unsigned i = 0; i < 8; ++i) f.memory.sq_write32(0xE0000000 + 4 * i, i + 1);
    f.memory.sq_flush(0xE0000040);
    CHECK(f.bus.read32(0x8C001040) == 1);
    CHECK(f.bus.read32(0x8C00105C) == 8);
    CHECK(f.bus.read32(0x8C000040) == 0);
    f.bus.write32(0xFF000000, 3);
    CHECK_THROWS_AS(f.memory.sq_flush(0xE0000040), mem::MmuFault);
    f.bus.write32(0xFF000010, 0x201);  // SQMD: privileged only
    CHECK_THROWS_AS(f.memory.sq_write32(0xE0000000, 0), mem::MmuFault);
}

TEST_CASE("WinCE MMU: UTLB address and data arrays synchronize dirty and valid bits") {
    MmuFixture f;
    f.bus.write32(0xF7000000, 0x0C000174);
    f.bus.write32(0xF6000000, 0x10100);  // clears D
    CHECK((f.bus.read32(0xF7000000) & 4) == 0);
    expect_fault(f, 0x10000, 0x80, true);
    f.bus.write32(0xF6000000, 0x10300);  // sets D
    f.memory.write32(0x10000, 99);
    CHECK(f.bus.read32(0x8C000000) == 99);
    f.bus.write32(0xF7000000, 0x0C000170);
    CHECK((f.bus.read32(0xF6000000) & 0x200) == 0);
}

TEST_CASE("WinCE MMU: SQ writes and PREF use different protection checks") {
    MmuFixture f;
    f.map(0, 0xE0000000, 0x0C000000, 0x174);
    f.memory.sq_write32(0xE0000000, 123);
    f.map(0, 0xE0000000, 0x0C000000, 0x150);  // user read-only, clean
    expect_fault(f, 0xE0000000, 0xC0, true);
    CHECK_THROWS_AS(f.memory.write8(0xE0000000, 1), mem::MmuFault);
    CHECK_THROWS_AS(f.memory.read32(0xE0000000), mem::MmuFault);
    CHECK_NOTHROW(f.memory.sq_flush(0xE0000000));
    CHECK(f.bus.read32(0x8C000000) == 123);
    f.bus.utlb().invalidate();
    CHECK_THROWS_AS(f.memory.sq_write32(0xE0000000, 5), mem::MmuFault);
    CHECK(f.bus.expevt == 0x60);
    CHECK_THROWS_AS(f.memory.sq_flush(0xE0000000), mem::MmuFault);
    CHECK(f.bus.expevt == 0x40);
}

TEST_CASE("WinCE MMU: mapped control registers OCRAM and replacement boundary") {
    MmuFixture f;
    f.map(0, 0x10000, 0x1F000000);
    f.bus.write32(0xFF00000C, 123);
    CHECK(f.memory.read32(0x1000C) == 123);
    CHECK_THROWS_AS(f.memory.read8(0x1000C), mem::MmuFault);
    CHECK_THROWS_AS(f.memory.read32(0x7C000000), mem::MmuFault);
    f.bus.write32(0xFF00001C, 0x20);
    f.memory.write32(0x7C000000, 456);
    CHECK(f.memory.read32(0x7C000000) == 456);
    CHECK_THROWS_AS(f.memory.fetch16(0x7C000000), mem::MmuFault);
    f.bus.write32(0xFF000010, 1 | (3 << 10) | (4 << 18));
    (void)f.memory.read32(0x1000C);
    CHECK((f.bus.read32(0xFF000010) & 0xFC00) == 0);
    f.cpu.sr = sh4::SR_MD;
    CHECK_THROWS_WITH(f.memory.write32(0xF6000080, 0),
                      "associative UTLB writes are not implemented in strict MMU mode");
    CHECK_THROWS_WITH(f.memory.write64(0xF6000080, 0),
                      "associative UTLB writes are not implemented in strict MMU mode");
}

#ifdef DREAM_DEV_INTERPRETER
TEST_CASE("WinCE MMU: interpreter fetches instructions from a virtual code page") {
    MmuFixture f;
    f.map(0);
    f.bus.write16(0x8C000000, 0xE02A);  // mov #42,r0
    f.bus.write16(0x8C000002, 0x000B);  // rts
    f.bus.write16(0x8C000004, 0x7001);  // add #1,r0 in the delay slot
    f.cpu.pc = 0x10000;
    f.cpu.pr = 0x20000;
    f.cpu.next_event = ~0ull;
    devinterp::Options options;
    options.call_translated = false;
    options.max_instructions = 10;
    CHECK(devinterp::run(f.cpu, f.memory, f.cpu.pr, options) == devinterp::Stop::Returned);
    CHECK(f.cpu.r[0] == 43);
}
#endif
