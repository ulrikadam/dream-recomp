// The console hardware behind a CE process: store-queue bursts after SetStoreQueueBase.
#include <cstdint>
#include <cstring>

#include "dream/runtime/wince/dc_hardware.h"
#include "dream/runtime/wince/process.h"
#include "dream/runtime/wince/system_services.h"

#include "doctest.h"

using dream::wince::DcHardware;

namespace {
void burst(DcHardware& hw, std::uint32_t address, std::uint32_t first) {
    for (std::uint32_t i = 0; i < 8; ++i) hw.store_queue_write(address + 4 * i, first + i);
    hw.store_queue_flush(address);
}
std::uint32_t ram_word(DcHardware& hw, std::uint32_t offset) {
    std::uint32_t v = 0;
    std::memcpy(&v, hw.system().memory.ram() + offset, 4);
    return v;
}
}  // namespace

TEST_CASE("wince hardware: the whole store-queue area follows SetStoreQueueBase") {
    DcHardware hw([] { return std::uint64_t{0}; });
    REQUIRE(hw.store_queue_base(0x0C000000u));
    // The first 1 MiB page, then one far beyond it: nk.exe maps each page on its TLB miss, so a
    // driver reaches every bus address from the base onward (Sega Rally 2's ddhal writes to
    // 0xE3000200 with base 0x0E000000 to reach 0x11000200).
    burst(hw, 0xE0000040u, 0x100);
    burst(hw, 0xE0F00020u, 0x200);
    CHECK(ram_word(hw, 0x000040) == 0x100);
    CHECK(ram_word(hw, 0x00005C) == 0x107);
    CHECK(ram_word(hw, 0xF00020) == 0x200);
    CHECK(ram_word(hw, 0xF0003C) == 0x207);
    CHECK(hw.system().memory.sq_tlb_flushes == 2);
    CHECK(hw.system().memory.sq_tlb_misses == 0);
}

TEST_CASE("wince hardware: locked pages follow the process memory they copy") {
    // MmProbeAndLockPages copies process memory into console RAM, where DMA reads it. SNDCORE
    // locks a sound buffer once and transfers from it again and again while the title goes on
    // writing it: the copy must take the bytes as they are at each transfer (and give back what
    // a transfer into RAM wrote), or Sega Rally 2's sound went out as silence.
    DcHardware hw([] { return std::uint64_t{0}; });
    dream::wince::ProcessMemory memory;
    memory.set_hardware(&hw);
    dream::wince::SystemServices services(memory);
    const auto buffer = memory.allocate(8192), mdl = memory.allocate(256);
    REQUIRE(buffer != 0);
    REQUIRE(mdl != 0);
    const std::uint32_t at = buffer + 0x10, count = 64;
    memory.write32(mdl + 12, at & ~0xFFFu);  // StartVa
    memory.write32(mdl + 16, count);         // ByteCount
    memory.write32(mdl + 20, at & 0xFFFu);   // ByteOffset
    for (std::uint32_t i = 0; i < count; i += 4) memory.write32(at + i, 0x11110000u + i);
    dream::sh4::Ctx cpu{};
    cpu.r[4] = mdl;
    std::uint32_t error = 0;
    REQUIRE(services.invoke(cpu, memory,
                            *dream::wince::SystemServices::resolve("MmProbeAndLockPages"), error));
    const std::uint32_t physical = (memory.read32(mdl + 24) << 12) + (at & 0xFFFu);
    CHECK(ram_word(hw, physical - 0x0C000000u) == 0x11110000u);  // copied at the lock
    for (std::uint32_t i = 0; i < count; i += 4) memory.write32(at + i, 0x22220000u + i);
    CHECK(ram_word(hw, physical - 0x0C000000u) == 0x11110000u);  // not yet
    services.sync_locked(physical + 8, 8, true);                 // a DMA reads two words
    CHECK(ram_word(hw, physical - 0x0C000000u + 8) == 0x22220008u);
    CHECK(ram_word(hw, physical - 0x0C000000u + 12) == 0x2222000Cu);
    CHECK(ram_word(hw, physical - 0x0C000000u + 16) == 0x11110010u);  // outside it: untouched
    std::uint32_t word = 0x33330000u;
    std::memcpy(hw.system().memory.ram() + (physical - 0x0C000000u + 20), &word, 4);
    services.sync_locked(physical + 20, 4, false);  // a DMA wrote one
    CHECK(memory.read32(at + 20) == 0x33330000u);
    services.sync_locked(0x0C000000u, 16, true);  // elsewhere in RAM: nothing locked there
    CHECK(memory.read32(at) == 0x22220000u);
}
