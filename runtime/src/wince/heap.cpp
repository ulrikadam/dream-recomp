#include "dream/runtime/wince/heap.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace dream::wince {
namespace {
std::uint32_t capacity(std::uint32_t size) {
    if (size > ProcessMemory::kBudget)
        throw MemoryError(8, "heap allocation exceeds budget");
    return (std::max(1u, size) + 7u) & ~7u;
}
// DREAM_WINCE_WATCH_FREE=address[:n]: report each heap free of a block holding that address, and
// stop the run at the nth (a bring-up aid for use-after-free hunts).
void watch_free(std::uint32_t p, std::uint32_t bytes) {
    static const char* v = std::getenv("DREAM_WINCE_WATCH_FREE");
    if (!v)
        return;
    static unsigned seen = 0;
    char* end = nullptr;
    const auto address = static_cast<std::uint32_t>(std::strtoul(v, &end, 0));
    const auto stop_at = end && *end == ':' ? std::strtoul(end + 1, nullptr, 0) : 0ul;
    if (address < p || address - p >= bytes)
        return;
    std::fprintf(stderr, "watch-free #%u: block 0x%08x (%u bytes)\n", ++seen, p, bytes);
    if (seen == stop_at)
        throw std::runtime_error("watched free");
}
}  // namespace
Heaps::Heaps(ProcessMemory& m) : memory_(m) {
    heaps_.emplace(kProcess, Heap{});
}
Heaps::Heap& Heaps::heap(std::uint32_t handle) {
    const auto it = heaps_.find(handle);
    if (it == heaps_.end())
        throw MemoryError(6, "invalid heap handle");
    return it->second;
}
Heaps::Block& Heaps::block(std::uint32_t h, std::uint32_t p, Family family) {
    heap(h);
    const auto it = blocks_.find(p);
    if (it == blocks_.end() || it->second.heap != h || it->second.family != family)
        throw MemoryError(87, "pointer does not belong to this heap allocation family");
    return it->second;
}
bool Heaps::add_arena(Heap& h, std::uint32_t size) {
    size = (size + 4095u) & ~4095u;
    if (h.maximum && size > h.maximum - h.committed)
        return false;
    const auto p = memory_.allocate(size);
    if (!p)
        return false;
    try {
        h.arenas.emplace(p, Arena{size, {{p, size}}});
    } catch (...) {
        memory_.release(p);
        throw;
    }
    h.committed += size;
    return true;
}
std::uint32_t Heaps::create(std::uint32_t initial, std::uint32_t maximum) {
    if (initial > ProcessMemory::kBudget || maximum > ProcessMemory::kBudget ||
        (maximum && initial > maximum))
        throw MemoryError(87, "invalid heap bounds");
    if (heaps_.size() >= 256 || next_ >= 0x1D200000)
        throw MemoryError(8, "heap handle limit");
    Heap h;
    h.maximum = maximum ? (maximum + 4095u) & ~4095u : 0;
    const auto handle = next_;
    auto it = heaps_.emplace(handle, std::move(h)).first;
    try {
        if (initial && !add_arena(it->second, initial))
            throw MemoryError(8, "cannot create initial heap arena");
    } catch (...) {
        heaps_.erase(it);
        throw;
    }
    next_ += 4;
    return handle;
}
void Heaps::destroy(std::uint32_t h) {
    auto& owner = heap(h);
    if (h == kProcess)
        throw MemoryError(87, "cannot destroy the process heap");
    for (const auto& [p, arena] : owner.arenas) {
        (void)arena;
        memory_.release(p);
    }
    std::erase_if(blocks_, [&](const auto& pair) { return pair.second.heap == h; });
    heaps_.erase(h);
}
std::uint32_t Heaps::allocate(std::uint32_t h, std::uint32_t size, bool zero, Family family) {
    auto& owner = heap(h);
    if (owner.maximum && size >= 0x7FFF8)
        throw MemoryError(8, "fixed-heap block exceeds CE limit");
    const auto bytes = capacity(size);
    if (blocks_.size() >= 65536)
        throw MemoryError(8, "heap block limit");
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        for (auto& [base, arena] : owner.arenas)
            for (auto it = arena.free.begin(); it != arena.free.end(); ++it) {
                if (it->second < bytes)
                    continue;
                const auto p = it->first, available = it->second;
                if (zero)
                    memory_.write_bytes(p, std::vector<std::uint8_t>(size));
                blocks_.emplace(p, Block{h, base, size, bytes, family});
                try {
                    if (available > bytes)
                        arena.free.emplace(p + bytes, available - bytes);
                } catch (...) {
                    blocks_.erase(p);
                    throw;
                }
                arena.free.erase(it);
                return p;
            }
        if (attempt)
            break;
        auto extent = std::max(65536u, (bytes + 4095u) & ~4095u);
        if (owner.maximum)
            extent = std::min(extent, owner.maximum - owner.committed);
        if (extent < bytes || !add_arena(owner, extent))
            break;
    }
    throw MemoryError(8, "heap exhausted");
}
void Heaps::coalesce(Arena& arena, std::uint32_t p, std::uint32_t size) {
    auto it = arena.free.emplace(p, size).first;
    if (it != arena.free.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second == it->first) {
            prev->second += it->second;
            arena.free.erase(it);
            it = prev;
        }
    }
    const auto next = std::next(it);
    if (next != arena.free.end() && it->first + it->second == next->first) {
        it->second += next->second;
        arena.free.erase(next);
    }
}
void Heaps::free(std::uint32_t h, std::uint32_t p, Family family) {
    const auto b = block(h, p, family);
    watch_free(p, b.capacity);
    auto& owner = heap(h);
    auto& arena = owner.arenas.at(b.arena);
    coalesce(arena, p, b.capacity);
    blocks_.erase(p);
    // Keep one arena for reuse; return other empty arenas to the process memory budget.
    if (owner.arenas.size() > 1 && arena.free.size() == 1 &&
        arena.free.begin()->second == arena.size) {
        owner.committed -= arena.size;
        memory_.release(b.arena);
        owner.arenas.erase(b.arena);
    }
}
std::uint32_t Heaps::size(std::uint32_t h, std::uint32_t p, Family family) const {
    if (!heaps_.contains(h))
        throw MemoryError(6, "invalid heap handle");
    const auto it = blocks_.find(p);
    if (it == blocks_.end() || it->second.heap != h || it->second.family != family)
        throw MemoryError(87, "pointer does not belong to this heap allocation family");
    return it->second.size;
}
bool Heaps::validate(std::uint32_t h, std::uint32_t p) const {
    if (!heaps_.contains(h))
        return false;
    if (!p)
        return true;  // Metadata is host-owned; guest writes cannot corrupt free lists.
    const auto it = blocks_.find(p);
    return it != blocks_.end() && it->second.heap == h && it->second.family == Family::Heap;
}
std::uint32_t Heaps::reallocate(std::uint32_t h, std::uint32_t p, std::uint32_t size, bool zero,
                                bool in_place, Family family) {
    auto& b = block(h, p, family);
    const auto bytes = capacity(size);
    if (heap(h).maximum && size >= 0x7FFF8)
        throw MemoryError(8, "fixed-heap block exceeds CE limit");
    auto& arena = heap(h).arenas.at(b.arena);
    const auto next = arena.free.find(p + b.capacity);
    if (bytes <= b.capacity || (next != arena.free.end() && next->second >= bytes - b.capacity)) {
        if (zero && size > b.size)
            memory_.write_bytes(p + b.size, std::vector<std::uint8_t>(size - b.size));
        if (bytes < b.capacity)
            coalesce(arena, p + bytes, b.capacity - bytes);
        else if (bytes > b.capacity) {
            const auto remaining = next->second - (bytes - b.capacity);
            if (remaining)
                arena.free.emplace(p + bytes, remaining);
            arena.free.erase(next);
        }
        b.size = size;
        b.capacity = bytes;
        return p;
    }
    if (in_place)
        throw MemoryError(8, "in-place heap growth unavailable");
    const auto data = memory_.read_bytes(p, std::min(size, b.size));
    const auto replacement = allocate(h, size, zero, family);
    try {
        memory_.write_bytes(replacement, data);
    } catch (...) {
        free(h, replacement, family);
        throw;
    }
    free(h, p, family);
    return replacement;
}
}  // namespace dream::wince
