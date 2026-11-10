#include "dream/runtime/wince/dc_hardware.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>

namespace dream::wince {
namespace {
// armcom.drv's header in sound RAM: the address of its work area, written once it is ready, and
// its write pointer in the ring of events it raises for the SH-4.
constexpr std::uint32_t kWorkArea = 0xEC, kRingPointer = 0xE4, kAramMask = (2u << 20) - 4u;
}  // namespace
DcHardware::DcHardware(std::function<std::uint64_t()> clock) : clock_(std::move(clock)) {
    // Built as boot_main builds it for a Katana title, minus the CPU side: the System is never
    // installed as the ABI hook target (nothing here is translated code).
    system_ = std::make_unique<System>();
    auto& sys = *system_;
    pvr_ = std::make_unique<pvr::Core>(sys.sched, sys.holly, sys.spg, sys.memory);
    sys.memory.map_mmio(pvr::Core::kRegBase, pvr::Core::kRegEnd, pvr_.get());
    sys.memory.map_mmio(pvr::Core::kFifoBase, pvr::Core::kFifoEnd, pvr_.get());
    dmac_ = std::make_unique<sh4::Dmac>(sys.memory);
    sysblock_ = std::make_unique<holly::SysBlock>(*dmac_, sys.holly, sys.memory, *pvr_);
    sys.memory.map_p4(sh4::Dmac::kBase, sh4::Dmac::kEnd, dmac_.get());
    sys.memory.map_mmio(holly::SysBlock::kBase, holly::SysBlock::kEnd, sysblock_.get());
    g2_ = std::make_unique<holly::G2Dma>(sys.sched, sys.holly, sys.memory);
    sys.memory.map_mmio(holly::G2Dma::kBase, holly::G2Dma::kEnd, g2_.get());
    auto store = [&](std::uint32_t base, std::uint32_t bytes, bool p4) {
        stored_.push_back(std::make_unique<mem::RegisterFile>(base, bytes / 4));
        if (p4)
            sys.memory.map_p4(base, base + bytes, stored_.back().get());
        else
            sys.memory.map_mmio(base, base + bytes, stored_.back().get());
    };
    store(0x005F6940u, 0x20, false);   // SB_PDTNRM .. SB_G2DTEXT
    store(0x005F7400u, 0x100, false);  // G1
    store(0x005F7C00u, 0x100, false);  // PVR DMA
    store(0xFF800000u, 0x50, true);    // BSC
    store(0xFF200000u, 0x24, true);    // UBC
    store(0xFFC00000u, 0x10, true);    // CPG
    store(0xFFC80000u, 0x40, true);    // RTC
    store(0xFFE80000u, 0x28, true);    // SCIF
    cache_arrays_ = std::make_unique<mem::NullRegion>();
    sys.memory.map_p4(0xF0000000u, 0xF6000000u, cache_arrays_.get());
    aica_ = std::make_unique<aica::Aica>(sys.sched, sys.holly, sys.memory);
    sys.memory.map_mmio(aica::Aica::kRegBase, aica::Aica::kRegEnd, aica_.get());
    rtc_ = std::make_unique<aica::Rtc>(static_cast<std::uint32_t>(std::time(nullptr)) +
                                       aica::Rtc::kUnixEpochOffset);
    sys.memory.map_mmio(aica::Rtc::kBase, aica::Rtc::kEnd, rtc_.get());
    rtc_tick_ = sys.sched.add("rtc", [this](std::uint64_t, std::uint64_t) {
        rtc_->tick();
        system_->sched.request(rtc_tick_, 200'000'000ull);
    });
    sys.sched.request(rtc_tick_, 200'000'000ull);
    maple_ = std::make_unique<maple::Bus>(sys.sched, sys.holly, sys.memory);
    auto pad = std::make_unique<maple::Controller>();
    pad_ = pad.get();
    auto card = std::make_unique<maple::MemoryCard>();
    card->format();
    card_ = card.get();
    maple_->attach_expansion(0, 0, std::move(card));
    maple_->attach(0, std::move(pad));
    sys.memory.map_mmio(maple::Bus::kBase, maple::Bus::kEnd, maple_.get());
    // The pad is read for the frame before the bus's hardware-triggered transfer.
    sys.spg.on_vblank_out = [this] {
        if (on_vblank)
            on_vblank();
        maple_->vblank();
    };
    sys.memory.on_device_access = [this] {
        if (clock_)
            advance_to(clock_());
    };
}

void DcHardware::boot_sound(const std::vector<std::uint8_t>& file) {
    auto& m = system_->memory;
    m.write32(0xA0702040u, 0x0F0Fu);  // as SYSSTART.EXE writes them
    m.write32(0xA0702044u, 0x0F1Fu);
    if (file.size() <= 12)
        throw std::runtime_error("armcom.drv is too short");
    const auto arm_reset = 0xA0702C00u;
    m.write32(arm_reset, (m.read32(arm_reset) & ~0xFFu) | 1u);  // hold the ARM
    const auto bytes = std::min<std::size_t>(file.size() - 12, 2u << 20);
    std::copy_n(file.begin() + 12, bytes, m.aram());
    // The event ring's start: where 0xE4 (the driver's write pointer in it) points at boot.
    std::uint32_t ring = 0;
    if (bytes >= kRingPointer + 4)
        std::memcpy(&ring, m.aram() + kRingPointer, 4);
    sound_ring_ = ring & kAramMask;
    m.write32(arm_reset, m.read32(arm_reset) & ~0xFFu);  // and let it run
}

void DcHardware::hand_over_sound_ring() {
    std::uint8_t* aram = system_->memory.aram();
    std::uint32_t area = 0;
    std::memcpy(&area, aram + kWorkArea, 4);
    if (!area)
        return;  // not ready yet
    // As SNDCORE.DLL opened first computes it: the work area + 8, the word aligned on 8.
    const std::uint32_t at = ((area + 8u) & ~4u) & kAramMask;
    std::memcpy(aram + at, &sound_ring_, 4);
    std::fprintf(stderr, "wince: sound driver ready: event ring at 0x%x, noted at 0x%x\n",
                 sound_ring_, at);
    sound_ring_ = 0;
}

maple::CardStatus DcHardware::load_card(const std::string& path) {
    return card_->load(path);
}

void DcHardware::advance_to(std::uint64_t cycles) {
    auto& sched = system_->sched;
    if (advancing_ || sched.now() >= cycles)
        return;
    advancing_ = true;  // an event may touch a device, which would come back here
    sched.advance_to(cycles);
    advancing_ = false;
    if (sound_ring_)
        hand_over_sound_ring();
}

namespace {
// DREAM_WINCE_TRACE_HW=physical address: print the first accesses to that register.
std::uint32_t traced() {
    static const std::uint32_t address = [] {
        const char* v = std::getenv("DREAM_WINCE_TRACE_HW");
        return v ? static_cast<std::uint32_t>(std::strtoul(v, nullptr, 0)) & 0x1FFFFFFFu : 0u;
    }();
    return address;
}
void trace(const char* what, std::uint32_t address, unsigned size, std::uint64_t value) {
    static unsigned shown = 0;
    if (traced() && (address & 0x1FFFFFFFu) == traced() && shown++ < 40)
        std::fprintf(stderr, "hw: %s%u 0x%08x = 0x%llx\n", what, size * 8, address,
                     static_cast<unsigned long long>(value));
}
}  // namespace

bool DcHardware::read(std::uint32_t address, unsigned size, std::uint64_t& value) {
    if (ports_.read(address, size, value))
        return true;
    ++accesses_;
    struct Report {
        std::uint32_t a;
        unsigned s;
        std::uint64_t& v;
        ~Report() { trace("read", a, s, v); }
    } report{address, size, value};
    auto& m = system_->memory;
    switch (size) {
        case 1:
            value = m.read8(address);
            return true;
        case 2:
            value = m.read16(address);
            return true;
        case 4:
            value = m.read32(address);
            return true;
        case 8:
            value = m.read64(address);
            return true;
        default:
            return false;
    }
}

bool DcHardware::write(std::uint32_t address, unsigned size, std::uint64_t value) {
    if (ports_.write(address, size, value))
        return true;
    ++accesses_;
    trace("write", address, size, value);
    auto& m = system_->memory;
    switch (size) {
        case 1:
            m.write8(address, static_cast<std::uint8_t>(value));
            return true;
        case 2:
            m.write16(address, static_cast<std::uint16_t>(value));
            return true;
        case 4:
            m.write32(address, static_cast<std::uint32_t>(value));
            return true;
        case 8:
            m.write64(address, value);
            return true;
        default:
            return false;
    }
}

bool DcHardware::store_queue_write(std::uint32_t address, std::uint32_t value) {
    system_->memory.sq_write32(address, value);
    return true;
}
bool DcHardware::store_queue_flush(std::uint32_t address) {
    ++accesses_;
    auto& m = system_->memory;
    if (sq_mapped_ && (address >> 26) == 0x38u) {  // the store-queue area
        ++m.sq_tlb_flushes;
        m.sq_flush_to(address, sq_base_ + ((address - 0xE0000000u) & 0x03FFFFE0u));
    } else {
        m.sq_flush(address);
    }
    return true;
}

bool DcHardware::store_queue_base(std::uint32_t physical) {
    // nk.exe keeps the address and maps a store-queue page on each TLB miss: the 1 MiB page
    // holding the faulting address goes to physical + its offset in the area (0x8C012560).
    sq_base_ = physical;
    sq_mapped_ = true;
    return true;
}
}  // namespace dream::wince
