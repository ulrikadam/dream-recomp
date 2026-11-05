// The console hardware behind a CE process: store-queue bursts after SetStoreQueueBase.
#include <cstdint>
#include <cstring>

#include "dream/runtime/wince/dc_hardware.h"

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
