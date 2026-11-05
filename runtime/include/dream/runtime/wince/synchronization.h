#pragma once

#include "dream/runtime/wince/process.h"

namespace dream::wince {
// Synchronization for the current one-thread process. No host thread, blocking wait or
// invented scheduler: finite unsatisfied waits advance virtual time; infinite ones fault.
class Synchronization {
public:
    explicit Synchronization(ProcessMemory& memory) : memory_(memory) {}
    static std::optional<std::uint32_t> resolve(const std::string& name);
    static bool handles(std::uint32_t target);
    bool invoke(sh4::Ctx& cpu, Memory& memory, std::uint32_t target, std::uint32_t& last_error);

private:
    enum class Kind { Event, Mutex };
    struct Object {
        Kind kind;
        bool manual = false, signaled = false;
        std::uint32_t owner = 0, recursion = 0;
    };
    struct Critical {
        // Legacy Dreamcast CE ABI: count, owner, opaque kernel token, needtrap.
        // Later CE releases add a fifth field; that ABI is not selected here.
        std::array<std::uint32_t, 4> words{};
    };
    ProcessMemory& memory_;
    std::map<std::uint32_t, std::shared_ptr<Object>> objects_;
    std::map<std::u16string, std::weak_ptr<Object>> names_;
    std::map<std::uint32_t, Critical> critical_;
    std::uint32_t next_handle_ = 0x1D000000, next_critical_ = 0x1D200000;

    std::u16string name_at(std::uint32_t address);
    std::uint32_t create(Object initial, std::uint32_t name, std::uint32_t& last_error);
    Object& object(std::uint32_t handle, Kind kind);
    std::optional<std::uint32_t> poll(std::span<const std::uint32_t> handles);
    void critical_access(std::uint32_t address);
    Critical& critical_at(std::uint32_t address);
    void store_critical(std::uint32_t address, const Critical& critical);
    void initialize_critical(std::uint32_t address);
};
}  // namespace dream::wince
