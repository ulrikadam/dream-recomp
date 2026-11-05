#pragma once
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "dream/runtime/sh4/ctx.h"

namespace dream::wince {
// Thrown on a guest thread's own host thread to end it (ExitThread, TerminateThread on itself).
struct ThreadExit {
    std::uint32_t code;
};

// Guest threads for the HLE process. Each guest thread runs on a host thread of its own, so a
// thread can block anywhere (inside a window procedure, a DLL initializer...) with its host
// stack intact; but only the thread holding the baton executes, so guest state needs no locks
// and a run is deterministic. Scheduling follows CE: the highest-priority ready thread runs,
// equal priorities take turns at the end of each time slice (`yield`). Time is one virtual clock
// in CPU cycles; when every thread waits, it jumps to the earliest deadline.
class Scheduler {
public:
    // Runs a guest thread from its initial context until its thread function returns; the
    // result is the exit code. Called on the thread's host thread.
    using Body = std::function<std::uint32_t(sh4::Ctx&)>;
    static constexpr std::uint32_t kMainThread = 2;              // GetCurrentThreadId of the first
    static constexpr std::uint32_t kCurrentThreadHandle = 0x41;  // GetCurrentThread()
    static constexpr std::uint32_t kStillActive = 259;
    // A created thread's PR: its thread function returning here ends the thread.
    static constexpr std::uint32_t kThreadReturn = 0x1EFFFFE0;
    static constexpr int kNormalPriority = 3;  // CE THREAD_PRIORITY_NORMAL (0 highest, 7 idle)

    explicit Scheduler(Body body) : body_(std::move(body)) {}
    // Host threads are detached: a process may end while others wait. The scheduler must
    // therefore outlive the process (allocate it and never delete it).
    ~Scheduler() = default;
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // The calling host thread becomes guest thread kMainThread, running `ctx`.
    void adopt_main(sh4::Ctx& ctx);
    // CreateThread: returns the new thread's handle and sets `id`.
    std::uint32_t create(const sh4::Ctx& initial, bool suspended, std::uint32_t& id);

    void yield();
    // Blocks the current thread until ready() holds (true) or the clock reaches `deadline`
    // (false). ready() is evaluated with the baton held, each time the thread runs again.
    bool wait(const std::function<bool()>& ready, std::optional<std::uint64_t> deadline);
    // Something was signalled: every waiting thread checks its condition again.
    void notify();
    [[noreturn]] void exit_current(std::uint32_t code);

    // Thread-handle services; the handle may be kCurrentThreadHandle. nullopt: invalid handle.
    std::optional<std::uint32_t> suspend(std::uint32_t handle);  // previous suspend count
    std::optional<std::uint32_t> resume(std::uint32_t handle);
    bool terminate(std::uint32_t handle, std::uint32_t code);
    std::optional<int> priority(std::uint32_t handle) const;
    bool set_priority(std::uint32_t handle, int priority);
    std::optional<std::uint32_t> exit_code(std::uint32_t handle) const;  // kStillActive if live
    bool owns_handle(std::uint32_t handle) const;
    bool exited(std::uint32_t handle) const;  // a thread handle is signalled once it exits
    bool close_handle(std::uint32_t handle);
    bool has_waiters() const;

    std::uint32_t current_id() const;
    sh4::Ctx& current_ctx() const;
    // For a stuck run: every thread's state, priority, deadline and PC, and who holds the baton.
    std::string describe() const;
    std::size_t created() const { return created_; }

    // Called with the baton each time a thread (re)starts running, before any of its guest
    // code: the kernel points per-thread state (the KData page) at it.
    std::function<void(std::uint32_t id)> on_switch;
    // The devices, while every thread waits: when they next need time to pass (nullopt: never),
    // and running them up to a time, which delivers their interrupts; true when that signalled
    // something a thread may be waiting for. Both run with the scheduler's lock held, so they
    // must not call back into it.
    std::function<std::optional<std::uint64_t>()> device_deadline;
    std::function<bool(std::uint64_t cycles)> run_devices;

private:
    enum class State { Ready, Running, Waiting, Exited };
    struct Thread {
        std::uint32_t id = 0;
        sh4::Ctx own{};  // a created thread's context (the main one lives elsewhere)
        sh4::Ctx* cpu = nullptr;
        State state = State::Ready;
        std::uint32_t suspended = 0, exit_code = kStillActive;
        int priority = kNormalPriority;
        std::optional<std::uint64_t> deadline;
        bool killed = false;
    };
    Body body_;
    mutable std::mutex mutex_;
    std::condition_variable turn_;
    std::map<std::uint32_t, std::unique_ptr<Thread>> threads_;  // by id
    std::map<std::uint32_t, std::uint32_t> handles_;            // handle -> id
    std::uint32_t running_ = 0, next_id_ = kMainThread + 1, next_handle_ = 0x1D800000;
    std::uint64_t now_ = 0;
    std::size_t created_ = 0;
    std::exception_ptr failure_;  // a thread's error, rethrown on the main thread

    static thread_local Thread* self_;
    Thread* lookup(std::uint32_t handle) const;
    Thread* pick(std::unique_lock<std::mutex>& lock);
    void switch_to(std::unique_lock<std::mutex>& lock, Thread* next);
    void await_turn(std::unique_lock<std::mutex>& lock);
    void host(Thread* thread);
};
}  // namespace dream::wince
