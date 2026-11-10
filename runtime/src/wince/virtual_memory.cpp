#include <algorithm>

#include "dream/runtime/wince/process.h"

namespace dream::wince {
namespace {
constexpr std::uint32_t kBegin = 0x20000000, kEnd = 0x40000000, kPage = 4096;
unsigned permissions(std::uint32_t p) {
    switch (p) {
        case 1:
            return 0;
        case 2:
            return ProcessMemory::Read;
        case 4:
            return ProcessMemory::Read | ProcessMemory::Write;
        case 0x10:
            return ProcessMemory::Execute;
        case 0x20:
            return ProcessMemory::Read | ProcessMemory::Execute;
        case 0x40:
            return ProcessMemory::Read | ProcessMemory::Write | ProcessMemory::Execute;
        default:
            throw MemoryError(87, "unsupported virtual page protection");
    }
}
std::uint32_t protection(unsigned p) {
    if (p & ProcessMemory::Execute)
        return p & ProcessMemory::Write ? 0x40u : p & ProcessMemory::Read ? 0x20u : 0x10u;
    return p & ProcessMemory::Write ? 4u : p & ProcessMemory::Read ? 2u : 1u;
}
std::uint32_t rounded_end(std::uint32_t at, std::uint32_t size) {
    const auto end = static_cast<std::uint64_t>(at) + size + kPage - 1;
    if (!size || at < kBegin || end > static_cast<std::uint64_t>(kEnd) + kPage - 1)
        throw MemoryError(487, "virtual range outside HLE reservation area");
    return static_cast<std::uint32_t>(end & ~(static_cast<std::uint64_t>(kPage) - 1));
}
}  // namespace

std::uint32_t ProcessMemory::virtual_alloc(std::uint32_t address, std::uint32_t size,
                                           std::uint32_t type, std::uint32_t protect) {
    const auto access = permissions(protect);
    if (!size || size > kBudget || !(type & 0x3000) || (type & ~0x103000u))
        throw MemoryError(87, "invalid VirtualAlloc flags or size");
    const bool commit = (type & 0x1000) != 0, reserve = (type & 0x2000) || !address;
    if ((type & 0x100000) && (!reserve || address))
        throw MemoryError(87, "TOP_DOWN requires an automatic reservation");
    std::uint32_t start = address & ~(reserve ? 65535u : 4095u), end = 0;
    auto available = [&](std::uint32_t at, std::uint32_t bytes) {
        if (at < kBegin || static_cast<std::uint64_t>(at) + bytes > kEnd)
            return false;
        for (const auto& [base, r] : reservations_)
            if (at < base + r.size && base < at + bytes)
                return false;
        for (const auto& [base, r] : regions_)
            if (at < base + r.bytes.size() && base < at + bytes)
                return false;
        return true;
    };
    if (!address) {
        const auto bytes = (size + 4095u) & ~4095u;
        if (type & 0x100000) {
            start = (kEnd - bytes) & ~65535u;
            while (start >= kBegin && !available(start, bytes)) start -= 65536;
        } else {
            start = kBegin;
            while (start < kEnd && !available(start, bytes)) start += 65536;
        }
        if (!available(start, bytes))
            throw MemoryError(8, "no virtual reservation range");
        end = start + bytes;
    } else
        end = rounded_end(address, size);
    auto reservation = reservations_.end();
    if (reserve) {
        if (end - start > kBudget || !available(start, end - start))
            throw MemoryError(487, "virtual reservation collision");
        if (reservations_.size() >= 1024)
            throw MemoryError(8, "virtual reservation limit");
    } else {
        reservation = reservations_.upper_bound(start);
        if (reservation == reservations_.begin())
            throw MemoryError(487, "commit outside a reservation");
        --reservation;
        if (start < reservation->first || end > reservation->first + reservation->second.size)
            throw MemoryError(487, "commit crosses a reservation");
    }
    // Stage new pages before publishing the reservation. A failing allocation commits nothing.
    std::map<std::uint32_t, Region> pages;
    std::uint32_t added = 0;
    if (commit)
        for (auto p = start; p < end; p += kPage) {
            if (!reserve && reservation->second.pages[(p - reservation->first) / kPage])
                continue;
            if (added > kBudget - used_ || kPage > kBudget - used_ - added)
                throw MemoryError(8, "virtual commit budget exceeded");
            pages.emplace(p, Region{std::vector<std::uint8_t>(kPage), access, false,
                                    reserve ? start : reservation->first});
            added += kPage;
        }
    if (reserve) {
        Reservation r{end - start, protect,
                      std::vector<std::uint32_t>((end - start) / kPage, commit ? protect : 0u)};
        reservation = reservations_.emplace(start, std::move(r)).first;
    } else if (commit) {
        for (const auto& [p, region] : pages) {
            (void)region;
            reservation->second.pages[(p - reservation->first) / kPage] = protect;
        }
    }
    regions_.merge(pages);
    used_ += added;
    forget();
    return start;
}
void ProcessMemory::virtual_free(std::uint32_t address, std::uint32_t size, std::uint32_t type) {
    if (type != 0x4000 && type != 0x8000)
        throw MemoryError(87, "invalid VirtualFree type");
    auto it = reservations_.upper_bound(address);
    if (it == reservations_.begin())
        throw MemoryError(487, "unknown virtual allocation");
    --it;
    auto& r = it->second;
    if (address < it->first || address >= it->first + r.size)
        throw MemoryError(487, "unknown virtual allocation");
    if (type == 0x8000 && (size || address != it->first))
        throw MemoryError(87, "release needs original base and zero size");
    if (!size && address != it->first)
        throw MemoryError(87, "whole decommit needs original base");
    const auto begin = address & ~4095u,
               end = size ? rounded_end(address, size) : it->first + r.size;
    if (end > it->first + r.size)
        throw MemoryError(487, "free crosses a reservation");
    forget();
    for (auto p = begin; p < end; p += kPage) {
        auto& state = r.pages[(p - it->first) / kPage];
        if (state) {
            regions_.erase(p);
            used_ -= kPage;
            state = 0;
        }
    }
    // A VirtualCopy window goes with the pages it was mapped over (MmUnmapIoSpace releases
    // them): left behind, it would capture the next reservation at that address.
    std::erase_if(windows_,
                  [&](const auto& w) { return w.first < end && begin < w.first + w.second.size; });
    if (type == 0x8000)
        reservations_.erase(it);
}
void ProcessMemory::virtual_protect(std::uint32_t address, std::uint32_t size,
                                    std::uint32_t protect, std::uint32_t old_pointer) {
    const auto access = permissions(protect), begin = address & ~4095u,
               end = rounded_end(address, size);
    if (!old_pointer || (old_pointer & 3))
        throw MemoryError(998, "invalid old-protection output");
    check(old_pointer, 4, Write);
    auto it = reservations_.upper_bound(begin);
    if (it == reservations_.begin())
        throw MemoryError(487, "protection outside a reservation");
    --it;
    if (begin < it->first || end > it->first + it->second.size)
        throw MemoryError(487, "protection crosses a reservation");
    for (auto p = begin; p < end; p += kPage)
        if (!it->second.pages[(p - it->first) / kPage])
            throw MemoryError(487, "protection includes uncommitted pages");
    const auto old = it->second.pages[(begin - it->first) / kPage];
    // Write the old protection before changing access, even if the output lives in this range.
    write32(old_pointer, old);
    forget();
    for (auto p = begin; p < end; p += kPage) {
        it->second.pages[(p - it->first) / kPage] = protect;
        regions_.at(p).permissions = access;
    }
}
std::array<std::uint32_t, 7> ProcessMemory::virtual_query(std::uint32_t address) const {
    if (address >= kEnd)
        throw MemoryError(87, "query outside HLE user address space");
    const auto begin = address & ~4095u;
    auto reservation = reservations_.upper_bound(begin);
    if (reservation != reservations_.begin()) {
        const auto& [base, r] = *std::prev(reservation);
        if (begin < base + r.size) {
            const auto first = (begin - base) / kPage, p = r.pages[first];
            auto next = first + 1;
            while (next < r.pages.size() && r.pages[next] == p) ++next;
            return {begin, base,   r.protection, (next - first) * kPage, p ? 0x1000u : 0x2000u,
                    p,     0x20000};
        }
    }
    auto region = regions_.upper_bound(address);
    if (region != regions_.begin()) {
        const auto& [base, r] = *std::prev(region);
        if (begin < base + r.bytes.size()) {
            const auto p = protection(r.permissions);
            const auto bytes =
                static_cast<std::uint32_t>((base + r.bytes.size() + 4095) & ~std::size_t{4095}) -
                begin;
            return {begin,
                    r.allocation_base,
                    p,
                    bytes,
                    0x1000,
                    p,
                    r.allocation ? 0x20000u : 0x1000000u};
        }
    }
    auto end = kEnd;
    if (reservation != reservations_.end())
        end = std::min(end, reservation->first);
    if (region != regions_.end())
        end = std::min(end, region->first & ~4095u);
    // Images can have sub-page headers. An otherwise unused remainder of that page is reported
    // as part of the mapped page; actual byte access still follows the stricter section bounds.
    if (end <= begin)
        end = begin + kPage;
    return {begin, 0, 0, end - begin, 0x10000, 0, 0};
}
}  // namespace dream::wince
