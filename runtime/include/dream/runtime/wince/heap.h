#pragma once
#include "dream/runtime/wince/process.h"

namespace dream::wince {
// Guest-only suballocation. Metadata never resides in guest memory. Families prevent LocalFree
// or free from releasing a module, stack, DirectX buffer, or another allocation API's block.
class Heaps {
public:
    enum class Family { Heap, Local, Crt };
    static constexpr std::uint32_t kProcess = 0x1D100000;
    explicit Heaps(ProcessMemory& memory);
    std::uint32_t create(std::uint32_t initial, std::uint32_t maximum);
    void destroy(std::uint32_t heap);
    std::uint32_t allocate(std::uint32_t heap, std::uint32_t size, bool zero,
                           Family family = Family::Heap);
    std::uint32_t reallocate(std::uint32_t heap, std::uint32_t pointer, std::uint32_t size,
                             bool zero, bool in_place, Family family = Family::Heap);
    void free(std::uint32_t heap, std::uint32_t pointer, Family family = Family::Heap);
    std::uint32_t size(std::uint32_t heap, std::uint32_t pointer,
                       Family family = Family::Heap) const;
    bool validate(std::uint32_t heap, std::uint32_t pointer) const;

private:
    struct Arena {
        std::uint32_t size;
        std::map<std::uint32_t, std::uint32_t> free;
    };
    struct Heap {
        std::uint32_t maximum = 0, committed = 0;
        std::map<std::uint32_t, Arena> arenas;
    };
    struct Block {
        std::uint32_t heap, arena, size, capacity;
        Family family;
    };
    ProcessMemory& memory_;
    std::map<std::uint32_t, Heap> heaps_;
    std::map<std::uint32_t, Block> blocks_;
    std::uint32_t next_ = kProcess + 4;
    Heap& heap(std::uint32_t handle);
    Block& block(std::uint32_t heap, std::uint32_t pointer, Family family);
    bool add_arena(Heap& heap, std::uint32_t size);
    static void coalesce(Arena& arena, std::uint32_t at, std::uint32_t size);
};
}  // namespace dream::wince
