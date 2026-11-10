#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "dream/runtime/aica/aica.h"
#include "dream/runtime/aica/rtc.h"
#include "dream/runtime/holly/g2dma.h"
#include "dream/runtime/holly/sysblock.h"
#include "dream/runtime/maple/maple.h"
#include "dream/runtime/pvr/core.h"
#include "dream/runtime/sh4/dmac.h"
#include "dream/runtime/system.h"
#include "dream/runtime/wince/hardware.h"

namespace dream::wince {
// The Dreamcast's devices behind a CE process, the same ones the Katana path runs: kernel-mode
// accesses at 0x80000000 and up (P1/P2 physical windows, P4 on-chip registers and store queues)
// go to a DcMemory bus carrying the PowerVR (registers, TA FIFO, video memory), the video timing
// (SPG) and the Holly interrupt controller. Device time follows the guest clock: before any
// device access, and whenever the runner calls advance_to (at each time slice).
//
// Only the bus is shared with the Katana path; the process's own memory is not on it (the
// process has its own address space), so a DMA a driver points at process memory is not seen
// yet. The SH-4 port A pair (AV cable) is answered here, as on the console with a VGA cable.
class DcHardware final : public Hardware {
public:
    // `clock` returns the running guest thread's cycle count.
    explicit DcHardware(std::function<std::uint64_t()> clock);
    bool read(std::uint32_t address, unsigned size, std::uint64_t& value) override;
    bool write(std::uint32_t address, unsigned size, std::uint64_t value) override;
    bool store_queue_write(std::uint32_t address, std::uint32_t value) override;
    bool store_queue_flush(std::uint32_t address) override;
    bool store_queue_base(std::uint32_t physical) override;
    void advance_to(std::uint64_t cycles);
    // What the OS image's SYSSTART.EXE does for sound before any title runs: the AICA's CD
    // output levels, then armcom.drv (the ARM7 sound driver, after a 12-byte header) copied to
    // the start of sound RAM with the ARM held in reset, and the ARM released. Then, once the
    // driver says it is ready (the address of its work area at 0xEC of sound RAM), the system
    // opens SNDCORE.DLL first, which leaves where the driver's event ring starts (what 0xE4,
    // the driver's write pointer in it, held at boot) at the work area + 8 for every later
    // client. A title's own SNDCORE only reads it there: left at zero, Sega Rally 2's sound
    // thread took the ring for 32 bytes at 0 and looped for ever once a stream started. Done
    // at the first device time after the driver is ready.
    void boot_sound(const std::vector<std::uint8_t>& armcom_drv);
    // The Maple bus has a standard controller in port A and a memory card in its first slot:
    // blank and formatted, kept in memory, until a card image is given (written back on saves).
    maple::CardStatus load_card(const std::string& path);
    // Called at each vertical blank (out), before the Maple bus runs: a display and its pad.
    // It runs inside device time, possibly with the scheduler's lock held: it must not wait on
    // or call into guest threads.
    std::function<void()> on_vblank;

    System& system() { return *system_; }
    pvr::Core& pvr() { return *pvr_; }
    aica::Aica& aica() { return *aica_; }
    holly::G2Dma& g2() { return *g2_; }
    maple::Bus& maple() { return *maple_; }
    maple::Controller& pad() { return *pad_; }
    maple::MemoryCard& card() { return *card_; }
    std::uint64_t accesses() const { return accesses_; }

private:
    std::function<std::uint64_t()> clock_;
    std::unique_ptr<System> system_;
    std::unique_ptr<pvr::Core> pvr_;
    // As boot_main wires them for a Katana title: channel-2 DMA and the system block (display
    // lists and textures), G2 DMA, and registers stored so programming them neither faults nor
    // aborts (G1, PVR DMA, DMA triggers, BSC, UBC, CPG, RTC, serial port, cache arrays).
    std::unique_ptr<sh4::Dmac> dmac_;
    std::unique_ptr<holly::SysBlock> sysblock_;
    std::unique_ptr<holly::G2Dma> g2_;
    std::vector<std::unique_ptr<mem::RegisterFile>> stored_;
    std::unique_ptr<mem::NullRegion> cache_arrays_;
    // The sound block: registers, sound RAM and the ARM7 running the driver the title loads,
    // and its real-time clock (the host date, one tick per guest second).
    std::unique_ptr<aica::Aica> aica_;
    std::unique_ptr<aica::Rtc> rtc_;
    int rtc_tick_ = -1;
    std::unique_ptr<maple::Bus> maple_;
    maple::Controller* pad_ = nullptr;
    maple::MemoryCard* card_ = nullptr;
    DreamcastHardware ports_;
    std::uint64_t accesses_ = 0;
    bool advancing_ = false;
    std::uint32_t sound_ring_ = 0;  // the driver's event ring start, until it is handed over
    void hand_over_sound_ring();
    bool sq_mapped_ = false;  // SetStoreQueueBase was called
    std::uint32_t sq_base_ = 0;
};
}  // namespace dream::wince
