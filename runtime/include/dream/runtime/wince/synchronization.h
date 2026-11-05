#pragma once

#include "dream/runtime/wince/process.h"
#include "dream/runtime/wince/threads.h"

namespace dream::wince {
// Synchronization objects. Without a scheduler the process has one thread: finite unsatisfied
// waits advance virtual time and infinite ones fault. With one (set_scheduler), waits and
// contended critical sections block the calling guest thread, every signal makes waiting
// threads check again, and thread handles are waitable (signalled when the thread exits).
class Synchronization {
public:
    explicit Synchronization(ProcessMemory& memory) : memory_(memory) {}
    void set_scheduler(Scheduler* scheduler) { scheduler_ = scheduler; }
    // The kernel signalling an event (an interrupt for an IST). `wake` makes waiting threads
    // check again; without it the caller does (the scheduler, from inside its own lock).
    bool set_event(std::uint32_t handle, bool wake);
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
    Scheduler* scheduler_ = nullptr;
    std::uint32_t me() const;  // the calling thread's id, owner of what it acquires
    void signal();             // waiting threads check their condition again
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
