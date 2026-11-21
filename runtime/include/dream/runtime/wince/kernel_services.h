#pragma once
#include "dream/runtime/wince/heap.h"

namespace dream::wince {
// Additional single-threaded COREDLL services. C runtime memory faults propagate; Win32-style
// functions return their documented failure sentinel and update the process last-error value.
class KernelServices {
public:
    explicit KernelServices(ProcessMemory& memory) : memory_(memory), heaps_(memory) {}
    static std::optional<std::uint32_t> resolve(const std::string& name);
    static bool handles(std::uint32_t target);
    bool invoke(sh4::Ctx& cpu, Memory& memory, std::uint32_t target, std::uint32_t& last_error);
    const std::vector<std::u16string>& debug_messages() const { return debug_; }

private:
    ProcessMemory& memory_;
    Heaps heaps_;
    std::vector<std::u16string> debug_;
    // rand's state. COREDLL keeps one per thread; one for the process, as a title that draws
    // from rand on one thread sees the same sequence either way.
    std::uint32_t rand_seed_ = 1;
};
}  // namespace dream::wince
