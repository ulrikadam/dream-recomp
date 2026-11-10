#include "dream/runtime/wince/synchronization.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <set>
#include <string_view>

namespace dream::wince {
namespace {
constexpr std::uint32_t kBase = 0x1F004000, kThread = 2;
enum Api : unsigned {
    CreateEvent,
    SetEvent,
    ResetEvent,
    WaitSingle,
    CloseHandle,
    CreateMutex,
    ReleaseMutex,
    InitializeCritical,
    DeleteCritical,
    EnterCritical,
    LeaveCritical,
    TryCritical,
    EventModify,
    PulseEvent,
    WaitMultiple,
    AddEventAccess
};
constexpr std::array<std::string_view, 16> kNames = {"CreateEventW",
                                                     "SetEvent",
                                                     "ResetEvent",
                                                     "WaitForSingleObject",
                                                     "CloseHandle",
                                                     "CreateMutexW",
                                                     "ReleaseMutex",
                                                     "InitializeCriticalSection",
                                                     "DeleteCriticalSection",
                                                     "EnterCriticalSection",
                                                     "LeaveCriticalSection",
                                                     "TryEnterCriticalSection",
                                                     "EventModify",
                                                     "PulseEvent",
                                                     "WaitForMultipleObjects",
                                                     "AddEventAccess"};
constexpr std::uint32_t kInfinite = 0xFFFFFFFFu, kTimeout = 258;
void wait_time(sh4::Ctx& cpu, std::uint32_t milliseconds) {
    if (milliseconds == kInfinite)
        throw std::runtime_error("unsatisfied infinite wait needs a WinCE thread scheduler");
    // CE clamps finite timeouts to the positive signed 32-bit range.
    const auto ticks = static_cast<std::uint64_t>(std::min(milliseconds, 0x7FFFFFFFu)) * 200000u;
    if (ticks > std::numeric_limits<std::uint64_t>::max() - cpu.cycles)
        throw std::runtime_error("WinCE virtual clock overflow");
    cpu.cycles += ticks;
}
}  // namespace

std::uint32_t Synchronization::me() const {
    return scheduler_ ? scheduler_->current_id() : kThread;
}
void Synchronization::signal() {
    if (scheduler_)
        scheduler_->notify();
}
bool Synchronization::set_event(std::uint32_t handle, bool wake) {
    const auto it = objects_.find(handle);
    if (it == objects_.end() || it->second->kind != Kind::Event)
        return false;
    it->second->signaled = true;
    if (wake)
        signal();
    return true;
}
std::optional<std::uint32_t> Synchronization::resolve(const std::string& name) {
    for (unsigned i = 0; i < kNames.size(); ++i)
        if (kNames[i] == name)
            return kBase + i * 4;
    return {};
}
bool Synchronization::handles(std::uint32_t target) {
    return target >= kBase && target < kBase + kNames.size() * 4 && !(target & 3u);
}
std::u16string Synchronization::name_at(std::uint32_t address) {
    if (!address)
        return {};
    if (address & 1u)
        throw MemoryError(998, "unaligned synchronization object name");
    std::u16string name;
    for (unsigned i = 0; i < 260; ++i) {
        const auto at = static_cast<std::uint64_t>(address) + i * 2;
        if (at > 0xFFFFFFFDull)
            throw MemoryError(998, "synchronization object name overflow");
        std::uint16_t c;
        try {
            c = memory_.read16(static_cast<std::uint32_t>(at));
        } catch (const std::runtime_error&) {
            throw MemoryError(998, "unreadable synchronization object name");
        }
        if (!c)
            return name;
        if (c == u'\\')
            throw MemoryError(123, "backslash in CE synchronization object name");
        name += static_cast<char16_t>(c);
    }
    throw MemoryError(123, "synchronization object name exceeds MAX_PATH");
}
std::uint32_t Synchronization::create(Object initial, std::uint32_t pointer,
                                      std::uint32_t& last_error) {
    const auto name = name_at(pointer);
    std::shared_ptr<Object> obj;
    if (const auto it = names_.find(name); !name.empty() && it != names_.end())
        obj = it->second.lock();
    const bool existed = obj != nullptr;
    if (obj && obj->kind != initial.kind)
        throw MemoryError(6, "name already belongs to a different synchronization object type");
    if (objects_.size() >= 1024 || next_handle_ >= 0x1D100000)
        throw MemoryError(8, "synchronization handle budget exhausted");
    if (!obj)
        obj = std::make_shared<Object>(initial);
    const auto h = next_handle_;
    objects_.emplace(h, obj);
    try {
        if (!name.empty() && !existed)
            names_.insert_or_assign(name, obj);
    } catch (...) {
        objects_.erase(h);
        throw;
    }
    next_handle_ += 4;
    last_error = existed ? 183u : 0u;
    return h;
}
Synchronization::Object& Synchronization::object(std::uint32_t h, Kind kind) {
    const auto it = objects_.find(h);
    if (it == objects_.end() || it->second->kind != kind)
        throw MemoryError(6, "invalid synchronization handle type");
    return *it->second;
}
std::optional<std::uint32_t> Synchronization::poll(std::span<const std::uint32_t> handles) {
    // Validate the entire list before an auto-reset event or a mutex can be consumed.
    std::set<std::uint32_t> seen;
    for (const auto h : handles) {
        if (!objects_.contains(h) && !(scheduler_ && scheduler_->owns_handle(h)))
            throw MemoryError(6, "invalid wait handle");
        if (!seen.insert(h).second)
            throw MemoryError(87, "duplicate wait handle");
    }
    for (unsigned i = 0; i < handles.size(); ++i) {
        if (!objects_.contains(handles[i])) {  // a thread handle
            if (scheduler_->exited(handles[i]))
                return i;
            continue;
        }
        auto& obj = *objects_.at(handles[i]);
        if (obj.kind == Kind::Event) {
            if (!obj.signaled)
                continue;
            if (!obj.manual)
                obj.signaled = false;
        } else {
            if (obj.owner && obj.owner != me())
                continue;
            if (obj.recursion == 0xFFFFFFFFu)
                throw MemoryError(8, "mutex recursion overflow");
            obj.owner = me();
            ++obj.recursion;
        }
        return i;
    }
    return {};
}
void Synchronization::critical_access(std::uint32_t address) {
    if (!address || (address & 3u))
        throw std::runtime_error("unaligned or null CE critical section");
    memory_.check(address, 16, ProcessMemory::Read | ProcessMemory::Write);
}
Synchronization::Critical& Synchronization::critical_at(std::uint32_t address) {
    critical_access(address);
    const auto it = critical_.find(address);
    if (it == critical_.end())
        throw std::runtime_error("uninitialized or deleted CE critical section");
    for (unsigned i = 0; i < 4; ++i)
        if (memory_.read32(address + i * 4) != it->second.words[i])
            throw std::runtime_error("CE critical section was copied, overwritten or freed");
    return it->second;
}
void Synchronization::store_critical(std::uint32_t address, const Critical& critical) {
    // Every caller preflights all 16 bytes before changing host or guest state.
    for (unsigned i = 0; i < 4; ++i) memory_.write32(address + i * 4, critical.words[i]);
}
void Synchronization::initialize_critical(std::uint32_t address) {
    critical_access(address);
    const auto it = critical_.lower_bound(address);
    if ((it != critical_.end() && it->first < static_cast<std::uint64_t>(address) + 16) ||
        (it != critical_.begin() &&
         static_cast<std::uint64_t>(std::prev(it)->first) + 16 > address))
        throw std::runtime_error("overlapping or repeated CE critical section initialization");
    if (critical_.size() >= 4096 || next_critical_ >= 0x1D300000)
        throw std::runtime_error("CE critical section budget exhausted");
    Critical state{{0, 0, next_critical_, 0}};
    critical_.emplace(address, state);
    store_critical(address, state);
    next_critical_ += 4;
}
bool Synchronization::invoke(sh4::Ctx& cpu, Memory& memory, std::uint32_t target,
                             std::uint32_t& last_error) {
    if (!handles(target))
        return false;
    if (&memory != &memory_)
        throw std::invalid_argument("synchronization service belongs to a different process");
    const auto op = static_cast<Api>((target - kBase) / 4);
    const auto a = cpu.r[4], b = cpu.r[5], c = cpu.r[6], d = cpu.r[7];
    auto& result = cpu.r[0];
    // Critical-section misuse has no Win32 last-error contract. Invalid lifetime/pointers
    // fault the guest rather than reporting successful acquisition.
    if (op >= InitializeCritical && op <= TryCritical) {
        if (op == InitializeCritical) {
            initialize_critical(a);
            return true;
        }
        auto& state = critical_at(a);
        if (op == DeleteCritical) {
            if (state.words[1])
                throw std::runtime_error("deleting an owned CE critical section");
            store_critical(a, Critical{});
            critical_.erase(a);
        } else if (op == LeaveCritical) {
            if (state.words[1] != me() || !state.words[0])
                throw std::runtime_error("leaving an unowned CE critical section");
            if (--state.words[0] == 0) {
                state.words[1] = 0;
                store_critical(a, state);
                signal();
            } else {
                store_critical(a, state);
            }
        } else {
            if (state.words[1] && state.words[1] != me()) {
                if (op == TryCritical) {
                    result = 0;
                    return true;
                }
                if (!scheduler_)
                    throw std::runtime_error(
                        "contended critical section needs a WinCE thread scheduler");
                // Another thread owns it: wait until it is free (or deleted meanwhile).
                scheduler_->wait(
                    [&] {
                        const auto it = critical_.find(a);
                        return it == critical_.end() || it->second.words[1] == 0;
                    },
                    std::nullopt);
            }
            auto& owned = critical_at(a);
            if (owned.words[0] == 0xFFFFFFFFu)
                throw std::runtime_error("CE critical section recursion overflow");
            owned.words[1] = me();
            ++owned.words[0];
            store_critical(a, owned);
            if (op == TryCritical)
                result = 1;
        }
        return true;
    }
    try {
        switch (op) {
            case CreateEvent:
            case CreateMutex:
                if (a)
                    throw MemoryError(120, "CE security attributes are not supported");
                if (op == CreateEvent)
                    result = create(Object{Kind::Event, b != 0, c != 0}, d, last_error);
                else
                    result = create(Object{Kind::Mutex, false, false, b ? me() : 0u, b ? 1u : 0u},
                                    c, last_error);
                break;
            case SetEvent:
            case ResetEvent:
            case PulseEvent:
            case EventModify: {
                auto& event = object(a, Kind::Event);
                const auto action = op == EventModify  ? b
                                    : op == SetEvent   ? 3u
                                    : op == ResetEvent ? 2u
                                                       : 1u;
                if (action < 1 || action > 3)
                    throw MemoryError(87, "invalid CE EventModify operation");
                // A pulse with waiting threads leaves the event set until one of them (auto
                // reset) or the next reset (manual) clears it: waiters check again only when
                // they run, so this approximates "release the waiters, then reset".
                event.signaled =
                    action == 3 || (action == 1 && scheduler_ && scheduler_->has_waiters());
                if (event.signaled)
                    signal();
                result = 1;
                break;
            }
            case ReleaseMutex: {
                auto& mutex = object(a, Kind::Mutex);
                if (mutex.owner != me())
                    throw MemoryError(288, "calling thread does not own this mutex");
                if (--mutex.recursion == 0) {
                    mutex.owner = 0;
                    signal();
                }
                result = 1;
                break;
            }
            case WaitSingle:
            case WaitMultiple: {
                std::array<std::uint32_t, 64> list{};
                unsigned count = 1;
                list[0] = a;
                if (op == WaitMultiple) {
                    if (!a || a > list.size() || c)
                        throw MemoryError(87, "CE waits require 1..64 handles and wait-all FALSE");
                    if (!b || (b & 3u))
                        throw MemoryError(998, "invalid wait handle array");
                    try {
                        memory_.check(b, a * 4, ProcessMemory::Read);
                        for (unsigned i = 0; i < a; ++i) list[i] = memory_.read32(b + i * 4);
                    } catch (const std::runtime_error&) {
                        throw MemoryError(998, "unreadable wait handle array");
                    }
                    count = a;
                }
                const std::span<const std::uint32_t> handles(list.data(), count);
                auto ready = poll(handles);
                const auto timeout = op == WaitMultiple ? d : b;
                if (!ready && scheduler_ && timeout) {
                    std::optional<std::uint64_t> deadline;
                    if (timeout != kInfinite)
                        deadline =
                            cpu.cycles +
                            static_cast<std::uint64_t>(std::min(timeout, 0x7FFFFFFFu)) * 200000u;
                    scheduler_->wait([&] { return (ready = poll(handles)).has_value(); }, deadline);
                }
                if (ready)
                    result = *ready;
                else {
                    if (!scheduler_)
                        wait_time(cpu, timeout);
                    result = kTimeout;
                }
                break;
            }
            case AddEventAccess:
                // (hEvent): a driver takes a reference to a caller's event, which it releases
                // with CloseHandle once it has set it (WDMLIB completing an overlapped request).
                // The caller's own handle must survive that.
                ++object(a, Kind::Event).references;
                result = 1;
                break;
            case CloseHandle: {
                const auto it = objects_.find(a);
                if (it == objects_.end())
                    throw MemoryError(6, "invalid CloseHandle synchronization object");
                if (it->second->references) {
                    --it->second->references;
                    result = 1;
                    break;
                }
                objects_.erase(it);
                std::erase_if(names_, [](const auto& pair) { return pair.second.expired(); });
                result = 1;
                break;
            }
            default:
                return false;
        }
    } catch (const MemoryError& error) {
        result = (op == WaitSingle || op == WaitMultiple) ? kInfinite : 0;
        last_error = error.code;
    } catch (const std::bad_alloc&) {
        result = (op == WaitSingle || op == WaitMultiple) ? kInfinite : 0;
        last_error = 8;
    }
    return true;
}
}  // namespace dream::wince
