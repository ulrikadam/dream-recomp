#include "dream/runtime/wince/process.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string_view>

#include "dream/runtime/wince/abi.h"
#include "dream/runtime/wince/kernel_services.h"
#include "dream/runtime/wince/modules.h"
#include "dream/runtime/wince/synchronization.h"
#include "dream/runtime/wince/system_services.h"

namespace dream::wince {
namespace {
void valid_range(std::uint32_t a, std::uint32_t n) {
    if (static_cast<std::uint64_t>(a) + n > 0x80000000ull) {
        char text[96];
        std::snprintf(text, sizeof text,
                      "WinCE process address range overflow at 0x%08x (%u bytes)", a, n);
        throw std::runtime_error(text);
    }
}
constexpr std::array<std::string_view, 34> kApis = {
    "GetTickCount", "GetLastError", "SetLastError", "Sleep", "LocalAlloc", "LocalFree", "TlsAlloc",
    "TlsFree", "TlsGetValue", "TlsSetValue", "GetCurrentProcessId", "GetCurrentThreadId",
    "ExitProcess", "LoadLibraryW", "LoadLibraryExW", "GetModuleHandleW", "GetProcAddressW",
    "GetProcAddressA", "CreateEventW", "SetEvent", "ResetEvent", "WaitForSingleObject",
    "CloseHandle",
    // CE's own TLS entry: TlsAlloc and TlsFree are
    // TlsCall(0, 0) and TlsCall(1, slot) in COREDLL.
    "TlsCall",
    // Modules are never unloaded: FreeLibrary
    // succeeds and the module stays mapped.
    "FreeLibrary", "GetModuleFileNameW",
    // Threads (26..33): need a Scheduler.
    "CreateThread", "ExitThread", "GetExitCodeThread", "ResumeThread", "SuspendThread",
    "TerminateThread", "SetThreadPriority", "GetThreadPriority"};
std::string api_name(std::string name) {
    if (name == "LoadLibrary" || name == "LoadLibraryEx" || name == "GetModuleHandle" ||
        name == "GetProcAddress" || name == "CreateEvent" || name == "CreateMutex" ||
        name == "GetModuleFileName")
        name += 'W';
    // A driver DLL loads like any other: there is no paging to keep it out of here.
    if (name == "LoadDriver")
        return "LoadLibraryW";
    // C++ operator new and delete are COREDLL's malloc and free.
    if (name == "??2@YAPAXI@Z")
        return "malloc";
    if (name == "??3@YAXPAX@Z")
        return "free";
    return name;
}
std::u16string wide_string(Memory& m, std::uint32_t p, unsigned limit = 260) {
    if (!p || (p & 1u) || static_cast<std::uint64_t>(p) + limit * 2 > 0x80000000ull)
        throw std::invalid_argument("invalid UTF-16 pointer");
    std::u16string result;
    for (unsigned i = 0; i < limit; ++i) {
        const auto v = m.read16(p + i * 2);
        if (!v)
            return result;
        result += static_cast<char16_t>(v);
    }
    throw std::invalid_argument("unterminated UTF-16 string");
}
std::string symbol_string(Memory& m, std::uint32_t p, bool wide) {
    std::string result;
    if (wide) {
        for (const auto c : wide_string(m, p)) {
            if (c > 127)
                throw std::invalid_argument("non-ASCII module/export name");
            result += static_cast<char>(c);
        }
        return result;
    }
    if (!p || static_cast<std::uint64_t>(p) + 260 > 0x80000000ull)
        throw std::invalid_argument("invalid export name pointer");
    for (unsigned i = 0; i < 260; ++i) {
        const auto v = m.read8(p + i);
        if (!v)
            return result;
        result += static_cast<char>(v);
    }
    throw std::invalid_argument("unterminated export name");
}
// "\Windows\FOO.DLL" -> "FOO.DLL": modules are found by file name; the disc is one folder.
std::string basename(const std::string& path) {
    const auto at = path.find_last_of("\\/");
    return at == std::string::npos ? path : path.substr(at + 1);
}
std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    return s;
}
}  // namespace

void ProcessMemory::install(const Image& image) {
    if (image.bytes.size() > kBudget - used_)
        throw std::runtime_error("WinCE memory budget exceeded");
    valid_range(image.base, static_cast<std::uint32_t>(image.bytes.size()));
    for (const auto& [base, r] : reservations_)
        if (image.base < base + r.size && base < image.base + image.bytes.size())
            throw std::runtime_error("WinCE image overlaps a virtual reservation");
    // Build the complete change separately: a collision never installs half an image.
    std::map<std::uint32_t, Region> added;
    auto add = [&](std::uint32_t offset, std::uint32_t size, unsigned permissions) {
        if (!size)
            return;
        if (offset > image.bytes.size() || size > image.bytes.size() - offset)
            throw std::runtime_error("invalid loaded-image region");
        const auto start = image.base + offset;
        for (const auto& [base, region] : regions_)
            if (start < base + region.bytes.size() && base < start + size)
                throw std::runtime_error("WinCE image address collision");
        for (const auto& [base, region] : added)
            if (start < base + region.bytes.size() && base < start + size)
                throw std::runtime_error("overlapping loaded-image regions");
        Region r{{image.bytes.begin() + offset, image.bytes.begin() + offset + size},
                 permissions,
                 false,
                 image.base};
        added.emplace(start, std::move(r));
    };
    add(0, image.headers_size, Read);
    for (const auto& s : image.sections) {
        unsigned permissions = 0;
        if (s.characteristics & 0x40000000u)
            permissions |= Read;
        if (s.characteristics & 0x80000000u)
            permissions |= Write;
        if (s.characteristics & 0x20000000u)
            permissions |= Execute;
        add(s.rva, s.size, permissions);
    }
    std::uint32_t bytes = 0;
    for (const auto& [base, region] : added) {
        (void)base;
        bytes += static_cast<std::uint32_t>(region.bytes.size());
    }
    regions_.merge(added);
    used_ += bytes;
    forget();
}

void ProcessMemory::map_window(std::uint32_t base, std::uint32_t size, std::uint32_t target) {
    valid_range(base, size);
    forget();
    // A later VirtualCopy over the same pages replaces the earlier mapping of them.
    std::erase_if(windows_, [&](const auto& w) {
        return base < w.first + w.second.size && w.first < base + size;
    });
    windows_.emplace(base, Window{size, target});
}
std::optional<std::uint32_t> ProcessMemory::window_target(std::uint32_t a) const {
    if (windows_.empty())
        return std::nullopt;
    auto it = windows_.upper_bound(a);
    if (it == windows_.begin())
        return std::nullopt;
    --it;
    if (a - it->first >= it->second.size)
        return std::nullopt;
    return it->second.target + (a - it->first);
}
bool ProcessMemory::map_fixed(std::uint32_t base, std::uint32_t size, unsigned permissions) {
    valid_range(base, size);
    for (const auto& [b, r] : regions_)
        if (base < b + r.bytes.size() && b < base + size)
            return false;
    regions_.emplace(base, Region{std::vector<std::uint8_t>(size), permissions, false, base});
    used_ += size;
    forget();
    return true;
}
std::uint32_t ProcessMemory::allocate(std::uint32_t bytes) {
    if (!bytes)
        bytes = 1;
    if (bytes > kBudget - used_ || allocation_count_ >= 4096)
        return 0;
    bytes = (bytes + 4095u) & ~4095u;
    if (bytes > kBudget - used_)
        return 0;
    std::uint32_t candidate = 0x10000000u;
    for (const auto& [base, r] : regions_) {
        if (base >= candidate + bytes)
            break;
        if (base + r.bytes.size() > candidate)
            candidate = (base + static_cast<std::uint32_t>(r.bytes.size()) + 4095u) & ~4095u;
    }
    if (candidate > 0x18000000u - bytes)
        return 0;
    regions_.emplace(candidate,
                     Region{std::vector<std::uint8_t>(bytes), Read | Write, true, candidate});
    used_ += bytes;
    ++allocation_count_;
    forget();
    return candidate;
}
bool ProcessMemory::release(std::uint32_t base) {
    const auto it = regions_.find(base);
    if (it == regions_.end() || !it->second.allocation)
        return false;
    forget();
    used_ -= static_cast<std::uint32_t>(it->second.bytes.size());
    --allocation_count_;
    regions_.erase(it);
    return true;
}
void ProcessMemory::check(std::uint32_t a, std::uint32_t size, unsigned permission) const {
    valid_range(a, size);
    auto fail = [&](const char* what) {
        char text[96];
        std::snprintf(text, sizeof text, "%s at 0x%08x", what, a);
        throw std::runtime_error(text);
    };
    while (size) {
        auto it = regions_.upper_bound(a);
        if (it == regions_.begin())
            fail("unmapped WinCE process address");
        --it;
        const auto offset = a - it->first;
        const auto& r = it->second;
        if (offset >= r.bytes.size())
            fail("unmapped WinCE process address");
        if ((r.permissions & permission) != permission)
            fail("WinCE page protection violation");
        const auto n = std::min(size, static_cast<std::uint32_t>(r.bytes.size()) - offset);
        a += n;
        size -= n;
    }
}
namespace {
// DREAM_WINCE_WATCH_WRITE=address[:n]: report each write covering that process address, and stop
// the run at the nth (a bring-up aid for finding what corrupts an object).
struct WriteWatch {
    std::uint32_t address = 0, stop_at = 0, seen = 0;
    bool on = false;
    WriteWatch() {
        if (const char* v = std::getenv("DREAM_WINCE_WATCH_WRITE")) {
            char* end = nullptr;
            address = static_cast<std::uint32_t>(std::strtoul(v, &end, 0));
            if (end && *end == ':')
                stop_at = static_cast<std::uint32_t>(std::strtoul(end + 1, nullptr, 0));
            on = true;
        }
    }
    void check(std::uint32_t a, std::uint32_t size, std::uint64_t value) {
        if (!on || address < a || address - a >= size)
            return;
        ++seen;
        std::fprintf(stderr, "watch-write #%u: 0x%08x (%u bytes at 0x%08x) = 0x%llx\n", seen,
                     address, size, a, static_cast<unsigned long long>(value));
        if (seen == stop_at)
            throw std::runtime_error("watched write");
    }
};
WriteWatch& write_watch() {
    static WriteWatch watch;
    return watch;
}
// Read once at start-up, so that stores do not ask on every call.
const bool watching = write_watch().on;
}  // namespace

bool ProcessMemory::touches_window(std::uint32_t a, std::uint32_t size) const {
    if (windows_.empty() || !size)
        return false;
    // Windows never overlap, so the last one starting in or before the range decides.
    auto it = windows_.upper_bound(a + (size - 1));
    if (it == windows_.begin())
        return false;
    --it;
    return it->first + it->second.size > a;
}
std::vector<std::uint8_t> ProcessMemory::read_bytes(std::uint32_t a, std::uint32_t size) const {
    if (size > kBudget)
        throw std::runtime_error("WinCE read exceeds memory budget");
    // A driver's DMA buffer (a VirtualCopy window) read by COREDLL's memcpy: byte by byte.
    if (touches_window(a, size)) {
        valid_range(a, size);
        std::vector<std::uint8_t> result(size);
        for (std::uint32_t i = 0; i < size; ++i)
            result[i] = static_cast<std::uint8_t>(load(a + i, 1));
        return result;
    }
    check(a, size, Read);
    std::vector<std::uint8_t> result(size);
    for (std::uint32_t i = 0; i < size;) {
        const auto it = std::prev(regions_.upper_bound(a + i));
        const auto offset = a + i - it->first;
        const auto count =
            std::min(size - i, static_cast<std::uint32_t>(it->second.bytes.size()) - offset);
        std::copy_n(it->second.bytes.begin() + offset, count, result.begin() + i);
        i += count;
    }
    return result;
}
void ProcessMemory::write_bytes(std::uint32_t a, std::span<const std::uint8_t> bytes) {
    if (bytes.size() > kBudget)
        throw std::runtime_error("WinCE write exceeds memory budget");
    if (bytes.size() > 8)  // single stores are reported by store()
        if (watching)
            write_watch().check(a, static_cast<std::uint32_t>(bytes.size()), bytes.size());
    if (touches_window(a, static_cast<std::uint32_t>(bytes.size()))) {
        valid_range(a, static_cast<std::uint32_t>(bytes.size()));
        for (std::size_t i = 0; i < bytes.size(); ++i)
            store(a + static_cast<std::uint32_t>(i), bytes[i], 1);
        return;
    }
    check(a, static_cast<std::uint32_t>(bytes.size()), Write);
    const bool outside = a - quiet_lo_ >= quiet_size_;
    bool changed = false;
    for (std::size_t i = 0; i < bytes.size();) {
        const auto at = a + static_cast<std::uint32_t>(i);
        auto it = std::prev(regions_.upper_bound(at));
        const auto offset = at - it->first;
        const auto count = std::min(bytes.size() - i, it->second.bytes.size() - offset);
        const auto to = it->second.bytes.begin() + static_cast<std::ptrdiff_t>(offset);
        const auto from = bytes.begin() + static_cast<std::ptrdiff_t>(i);
        changed = changed || (outside && !std::equal(from, from + count, to));
        std::copy_n(from, count, to);
        i += count;
    }
    outside_writes_ += changed;  // see store()
}
void ProcessMemory::remember(std::uint32_t a, Hit& into) const {
    const auto it = regions_.upper_bound(a);
    if (it == regions_.begin())
        return;
    auto& [base, region] = *std::prev(it);
    const auto bytes = static_cast<std::uint32_t>(region.bytes.size());
    if (a - base >= bytes || touches_window(base, bytes))
        return;  // a window over it takes its accesses: those must take the long way
    into = {base, bytes, const_cast<std::uint8_t*>(region.bytes.data()), region.permissions};
}
std::uint64_t ProcessMemory::load(std::uint32_t a, unsigned size, unsigned permission) const {
    if (a & (std::min(size, 4u) - 1u))
        throw std::runtime_error("unaligned SH-4 process read");
    // Most accesses fall in a region one did lately: no lookup then.
    Hit& hit = hit_for(a, permission == Execute);
    if (const auto offset = a - hit.base; offset < hit.size && hit.size - offset >= size &&
                                          (hit.permissions & permission) == permission) {
        std::uint64_t value = 0;
        std::memcpy(&value, hit.bytes + offset, size);  // little-endian, as the guest
        return value;
    }
    ++(permission == Execute ? slow_fetches : slow_reads);
    if (size == 4 && permission == Read)
        if (const auto it = fixed_.find(a); it != fixed_.end())
            return it->second;
    if (a >= 0x80000000u && hardware_ && permission == Read) {
        std::uint64_t value = 0;
        if (hardware_->read(a, size, value)) {
            ++device_reads_;
            return value;
        }
    }
    if (const auto target = window_target(a); target && permission == Read) {
        ++device_reads_;
        std::uint64_t value = 0;
        if (!hardware_ || !hardware_->read(*target, size, value))
            throw std::runtime_error("VirtualCopy window onto hardware that is not emulated");
        return value;
    }
    check(a, size, permission);
    remember(a, hit);
    std::uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) {
        const auto it = std::prev(regions_.upper_bound(a + i));
        value |= static_cast<std::uint64_t>(it->second.bytes[a + i - it->first]) << (8 * i);
    }
    return value;
}
void ProcessMemory::store(std::uint32_t a, std::uint64_t v, unsigned size) {
    if (watching)
        write_watch().check(a, size, v);
    if (a & (std::min(size, 4u) - 1u))
        throw std::runtime_error("unaligned SH-4 process write");
    // Outside the quiet window, a write counts as work if it changes memory (storing the value
    // already there, as a status variable each turn of a busy-wait does, changes nothing).
    const bool outside = a - quiet_lo_ >= quiet_size_;
    Hit& hit = hit_for(a);
    if (const auto offset = a - hit.base;
        offset < hit.size && hit.size - offset >= size && (hit.permissions & Write)) {
        if (outside && std::memcmp(hit.bytes + offset, &v, size) != 0)  // little-endian
            ++outside_writes_;
        std::memcpy(hit.bytes + offset, &v, size);
        return;
    }
    ++slow_writes;
    if (a >= 0x80000000u && hardware_ && hardware_->write(a, size, v)) {
        outside_writes_ += outside;  // the hardware: work
        return;
    }
    if (const auto target = window_target(a)) {
        outside_writes_ += outside;
        if (!hardware_ || !hardware_->write(*target, size, v))
            throw std::runtime_error("VirtualCopy window onto hardware that is not emulated");
        return;
    }
    std::array<std::uint8_t, 8> bytes{};
    for (unsigned i = 0; i < size; ++i) bytes[i] = static_cast<std::uint8_t>(v >> (8 * i));
    write_bytes(a, std::span<const std::uint8_t>(bytes.data(), size));
    remember(a, hit);
}
// load()'s first test, without the call: the interpreter's data reads nearly all end here.
template <typename T>
bool ProcessMemory::quick_read(std::uint32_t a, T& value) const {
    constexpr unsigned size = sizeof(T);
    const Hit& hit = hit_for(a);
    const auto offset = a - hit.base;
    if ((a & (std::min(size, 4u) - 1u)) || offset >= hit.size || hit.size - offset < size ||
        !(hit.permissions & Read))
        return false;
    std::memcpy(&value, hit.bytes + offset, size);  // little-endian, as the guest
    return true;
}
std::uint8_t ProcessMemory::read8(std::uint32_t a) {
    std::uint8_t v;
    return quick_read(a, v) ? v : static_cast<std::uint8_t>(load(a, 1));
}
std::uint16_t ProcessMemory::read16(std::uint32_t a) {
    std::uint16_t v;
    return quick_read(a, v) ? v : static_cast<std::uint16_t>(load(a, 2));
}
std::uint16_t ProcessMemory::fetch16(std::uint32_t a) {
    return static_cast<std::uint16_t>(load(a, 2, Execute));
}
bool ProcessMemory::code_span(std::uint32_t a, CodeSpan& span) {
    // The region fetch16 would read from, when it is executable and no window lies over it.
    const auto it = regions_.upper_bound(a);
    if (it == regions_.begin())
        return false;
    const auto& [base, region] = *std::prev(it);
    const auto bytes = static_cast<std::uint32_t>(region.bytes.size());
    if (a - base >= bytes || !(region.permissions & Execute) || touches_window(base, bytes))
        return false;
    span = {region.bytes.data(), base, bytes, &generation_, generation_};
    return true;
}
std::uint32_t ProcessMemory::read32(std::uint32_t a) {
    std::uint32_t v;
    return quick_read(a, v) ? v : static_cast<std::uint32_t>(load(a, 4));
}
std::uint64_t ProcessMemory::read64(std::uint32_t a) {
    std::uint64_t v;
    return quick_read(a, v) ? v : load(a, 8);
}
// store()'s first test, without the call (and with the size known): same counting.
template <typename T>
bool ProcessMemory::quick_write(std::uint32_t a, T v) {
    constexpr unsigned size = sizeof(T);
    Hit& hit = hit_for(a);
    const auto offset = a - hit.base;
    if (watching || (a & (std::min(size, 4u) - 1u)) || offset >= hit.size ||
        hit.size - offset < size || !(hit.permissions & Write))
        return false;
    if (a - quiet_lo_ >= quiet_size_ && std::memcmp(hit.bytes + offset, &v, size) != 0)
        ++outside_writes_;
    std::memcpy(hit.bytes + offset, &v, size);  // little-endian
    return true;
}
void ProcessMemory::write8(std::uint32_t a, std::uint8_t v) {
    if (!quick_write(a, v))
        store(a, v, 1);
}
void ProcessMemory::write16(std::uint32_t a, std::uint16_t v) {
    if (!quick_write(a, v))
        store(a, v, 2);
}
void ProcessMemory::write32(std::uint32_t a, std::uint32_t v) {
    if (!quick_write(a, v))
        store(a, v, 4);
}
void ProcessMemory::write64(std::uint32_t a, std::uint64_t v) {
    if (!quick_write(a, v))
        store(a, v, 8);
}
void ProcessMemory::sq_write32(std::uint32_t a, std::uint32_t v) {
    ++outside_writes_;  // to the hardware
    if (!hardware_ || !hardware_->store_queue_write(a, v))
        throw std::runtime_error("store queues unavailable in HLE process");
}
void ProcessMemory::sq_flush(std::uint32_t a) {
    if (!hardware_ || !hardware_->store_queue_flush(a))
        throw std::runtime_error("store queues unavailable in HLE process");
}

std::optional<std::uint32_t> Kernel::resolve(const Import& imp) const {
    // WDMLIB's DMA buffer calls go through the device manager's API set, which no process
    // provides here: the kernel serves them from the emulated console RAM instead.
    if (lower(imp.dll) == "wdmlib.dll") {
        if (!imp.ordinal &&
            ((imp.name.starts_with("Hal") && imp.name.find("CommonBuffer") != std::string::npos) ||
             imp.name == "MmProbeAndLockPages" || imp.name == "MmUnlockPages" ||
             imp.name == "HalMapInterrupt"))
            return SystemServices::resolve(imp.name);
        return {};
    }
    if (lower(imp.dll) != "coredll.dll")
        return {};
    auto name = imp.name;
    if (imp.ordinal) {
        const auto it = ordinals_.find(*imp.ordinal);
        if (it == ordinals_.end())
            return {};
        name = it->second;
    }
    name = api_name(name);
    for (std::size_t i = 0; i < kApis.size(); ++i)
        if (kApis[i] == name)
            return kTrapBase + static_cast<std::uint32_t>(i) * 4;
    if (const auto sync = Synchronization::resolve(name))
        return sync;
    if (const auto system = SystemServices::resolve(name))
        return system;
    return KernelServices::resolve(name);
}
void Kernel::bind_ordinal(std::uint16_t ordinal, const std::string& name) {
    if (std::find(kApis.begin(), kApis.end(), api_name(name)) == kApis.end() &&
        !KernelServices::resolve(api_name(name)) && !Synchronization::resolve(api_name(name)) &&
        !SystemServices::resolve(api_name(name)))
        throw std::invalid_argument("unsupported COREDLL ordinal target");
    const auto it = ordinals_.find(ordinal);
    if (it != ordinals_.end() && it->second != name)
        throw std::invalid_argument("conflicting COREDLL ordinal");
    ordinals_[ordinal] = name;
}
void Kernel::attach_modules(Modules& modules, std::uint32_t main,
                            std::function<bool(std::uint32_t, std::uint32_t, unsigned)> entry) {
    modules_ = &modules;
    main_module_ = main;
    entry_call_ = std::move(entry);
}
void Kernel::set_guest_call(GuestCall call) {
    guest_call_ = std::move(call);
    if (system_)
        system_->set_guest_call(guest_call_);
}
void Kernel::set_class_resolver(ClassResolver resolver) {
    class_resolver_ = std::move(resolver);
    if (system_)
        system_->set_class_resolver(class_resolver_);
}
void Kernel::make_system(ProcessMemory& memory) {
    system_ = std::make_unique<SystemServices>(memory);
    system_->set_root(file_root_);
    system_->set_guest_call(guest_call_);
    system_->set_class_resolver(class_resolver_);
    system_->set_device_host(device_host_);
    system_->set_event_signaller([this](std::uint32_t event) {
        if (synchronization_)
            synchronization_->set_event(event, true);
    });
}
SystemServices* Kernel::system_services() {
    if (!system_ && process_)
        make_system(*process_);
    return system_.get();
}
void Kernel::set_device_host(DeviceHost* host) {
    device_host_ = host;
    if (system_)
        system_->set_device_host(host);
}
Kernel::Kernel() = default;
Kernel::~Kernel() = default;
const std::vector<std::u16string>& Kernel::debug_messages() const {
    static const std::vector<std::u16string> empty;
    return services_ ? services_->debug_messages() : empty;
}
std::uint32_t Kernel::thread_id() const {
    return scheduler_ ? scheduler_->current_id() : Scheduler::kMainThread;
}
std::uint32_t Kernel::last_error() const {
    const auto it = errors_.find(thread_id());
    return it == errors_.end() ? 0 : it->second;
}
void Kernel::set_scheduler(Scheduler* scheduler, ProcessMemory* memory) {
    scheduler_ = scheduler;
    if (synchronization_)
        synchronization_->set_scheduler(scheduler);
    process_ = memory;
    if (!scheduler || !memory)
        return;
    // CE reserves TLS slots 0-2 (message queue, C runtime, kernel): TlsAlloc starts at 3.
    for (std::uint32_t slot = 0; slot < 3; ++slot) tls_[slot] = 0;
    if (!memory->map_fixed(kKData & ~0xFFFu, 0x1000, ProcessMemory::Read | ProcessMemory::Write))
        throw std::runtime_error("KData page overlaps the process");
    scheduler->on_switch = [this](std::uint32_t id) { point_kdata(id); };
    point_kdata(scheduler->current_id());
}
bool Kernel::deliver_interrupts(std::uint32_t istnrm, std::uint32_t istext, std::uint32_t isterr,
                                bool wake) {
    if (!system_ || !synchronization_)
        return false;
    bool any = false;
    for (const auto event : system_->due_interrupts(istnrm, istext, isterr))
        any |= synchronization_->set_event(event, wake);
    return any;
}
std::map<std::uint32_t, std::uint64_t> Kernel::interrupt_counts() const {
    return system_ ? system_->interrupt_counts() : std::map<std::uint32_t, std::uint64_t>{};
}
std::uint32_t Kernel::tls_block(std::uint32_t thread) {
    if (const auto it = tls_blocks_.find(thread); it != tls_blocks_.end())
        return it->second;
    const auto block = process_ ? process_->allocate(64 * 4) : 0;
    if (!block)
        throw std::runtime_error("no guest memory for a thread's TLS array");
    tls_blocks_[thread] = block;
    return block;
}
// KDataStruct: lpvTls at +0, then ahSys[]: SH_CURTHREAD at +8, SH_CURPROC at +12.
void Kernel::point_kdata(std::uint32_t thread) {
    process_->write32(kKData, tls_block(thread));
    process_->write32(kKData + 8, thread);
    process_->write32(kKData + 12, kProcessId);
}
bool Kernel::invoke(sh4::Ctx& c, Memory& memory, std::uint32_t target) {
    std::uint32_t& last_error_ = errors_[thread_id()];  // GetLastError is per thread
    // Preserve the original v2 event trap addresses while sharing typed handles with mutexes.
    if (target >= kTrapBase + 18 * 4 && target <= kTrapBase + 22 * 4 && !(target & 3u))
        target = *Synchronization::resolve(std::string(kApis[(target - kTrapBase) / 4]));
    if (auto* as_process = dynamic_cast<ProcessMemory*>(&memory); as_process && !system_)
        make_system(*as_process);
    // CloseHandle serves file and thread handles as well as synchronization objects.
    if (target == *Synchronization::resolve("CloseHandle") &&
        ((system_ && system_->close(c.r[4])) || (scheduler_ && scheduler_->close_handle(c.r[4])))) {
        c.r[0] = 1;
        return true;
    }
    if (Synchronization::handles(target)) {
        auto* process = dynamic_cast<ProcessMemory*>(&memory);
        if (!process)
            throw std::invalid_argument("COREDLL requires a WinCE process address space");
        if (!synchronization_) {
            synchronization_ = std::make_unique<Synchronization>(*process);
            synchronization_->set_scheduler(scheduler_);
        }
        return synchronization_->invoke(c, memory, target, last_error_);
    }
    if (SystemServices::handles(target)) {
        if (!system_)
            throw std::invalid_argument("COREDLL requires a WinCE process address space");
        return system_->invoke(c, memory, target, last_error_);
    }
    if (target == kTrapBase + 4 * 4)
        target = *KernelServices::resolve("LocalAlloc");
    if (target == kTrapBase + 5 * 4)
        target = *KernelServices::resolve("LocalFree");
    if (KernelServices::handles(target)) {
        auto* process = dynamic_cast<ProcessMemory*>(&memory);
        if (!process)
            throw std::invalid_argument("COREDLL requires a WinCE process address space");
        if (!services_)
            services_ = std::make_unique<KernelServices>(*process);
        return services_->invoke(c, memory, target, last_error_);
    }
    if (target < kTrapBase || target >= kTrapBase + kApis.size() * 4 || (target & 3u))
        return false;
    auto* process = dynamic_cast<ProcessMemory*>(&memory);
    if (!process)
        throw std::invalid_argument("COREDLL requires a WinCE process address space");
    auto& result = c.r[0];
    auto a = c.r[4], b = c.r[5];
    auto op = (target - kTrapBase) / 4;
    if (op == 23) {  // TlsCall(TLS_FUNCALLOC = 0 | TLS_FUNCFREE = 1, slot)
        if (a > 1) {
            result = 0xFFFFFFFFu;
            last_error_ = 87;
            return true;
        }
        op = a == 0 ? 6 : 7;
        a = b;
    }
    if (op == 25) {  // GetModuleFileNameW(hModule, lpFilename, nSize): "\CD-ROM\NAME"
        result = 0;
        std::string name;
        const auto wanted = a ? a : main_module_;
        if (modules_)
            for (const auto& [key, module] : modules_->all())
                if (module.image.base == wanted)
                    name = key;
        if (name.empty() || !b) {
            last_error_ = 6;
            return true;
        }
        std::u16string path = u"\\CD-ROM\\";
        for (const auto ch : name)
            path += static_cast<char16_t>(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
        const auto size = c.r[6];
        if (!size)
            return true;
        const auto count = std::min<std::size_t>(path.size(), size - 1);
        for (std::size_t i = 0; i < count; ++i)
            memory.write16(b + static_cast<std::uint32_t>(i) * 2, path[i]);
        memory.write16(b + static_cast<std::uint32_t>(count) * 2, 0);
        result = static_cast<std::uint32_t>(count);
        return true;
    }
    if (op >= 26 && op <= 33) {
        if (!scheduler_)
            throw std::runtime_error("guest threads need a scheduler (dream-wince --run)");
        auto invalid = [&] {
            last_error_ = 6;  // ERROR_INVALID_HANDLE
        };
        switch (op) {
            case 26: {
                // CreateThread(lpsa, cbStack, lpStartAddress, lpParameter, dwCreationFlags,
                // lpThreadId). The stack is the requested size in 64 KiB steps, 64 KiB to 1 MiB.
                const auto flags = argument(c, memory, 4), id_out = argument(c, memory, 5);
                const auto size =
                    std::clamp<std::uint32_t>((b + 0xFFFFu) & ~0xFFFFu, 0x10000u, 0x100000u);
                const auto stack = process->allocate(size);
                if (!stack) {
                    last_error_ = 8;
                    result = 0;
                    break;
                }
                sh4::Ctx thread{};
                thread.sr = c.sr;
                thread.fpscr = c.fpscr;
                thread.cycles = c.cycles;
                thread.next_event = c.next_event;
                thread.pc = c.r[6];
                thread.r[4] = c.r[7];
                thread.r[15] = stack + size - 16;  // the four home slots of its first call
                thread.pr = Scheduler::kThreadReturn;
                std::uint32_t id = 0;
                result = scheduler_->create(thread, (flags & 4u) != 0, id);  // CREATE_SUSPENDED
                if (id_out)
                    memory.write32(id_out, id);
                break;
            }
            case 27:  // ExitThread(code): the first thread's exit ends the process
                if (thread_id() == Scheduler::kMainThread)
                    throw ProcessExit{a};
                scheduler_->exit_current(a);
            case 28:  // GetExitCodeThread(h, lpExitCode)
                if (const auto code = scheduler_->exit_code(a)) {
                    memory.write32(b, *code);
                    result = 1;
                } else {
                    invalid();
                    result = 0;
                }
                break;
            case 29:
            case 30: {  // ResumeThread(h) / SuspendThread(h): previous suspend count
                const auto previous = op == 29 ? scheduler_->resume(a) : scheduler_->suspend(a);
                result = previous ? *previous : 0xFFFFFFFFu;
                if (!previous)
                    invalid();
                break;
            }
            case 31:  // TerminateThread(h, code)
                if (scheduler_->owns_handle(a) && thread_id() == Scheduler::kMainThread &&
                    a == Scheduler::kCurrentThreadHandle)
                    throw ProcessExit{b};
                result = scheduler_->terminate(a, b) ? 1 : 0;
                if (!result)
                    invalid();
                break;
            case 32:  // SetThreadPriority(h, nPriority)
                result = scheduler_->set_priority(a, static_cast<int>(b)) ? 1 : 0;
                if (!result)
                    last_error_ = 87;
                break;
            case 33: {  // GetThreadPriority(h)
                const auto priority = scheduler_->priority(a);
                result = priority ? static_cast<std::uint32_t>(*priority) : 0x7FFFFFFFu;
                if (!priority)
                    invalid();
                break;
            }
        }
        return true;
    }
    if (op == 24) {  // FreeLibrary(hModule)
        result = a ? 1 : 0;
        if (!a)
            last_error_ = 6;
        return true;
    }
    switch (op) {
        case 0:
            result = static_cast<std::uint32_t>(c.cycles / 200000u);
            break;
        case 1:
            result = last_error_;
            break;
        case 2:
            last_error_ = a;
            break;
        case 3:  // Sleep: with threads, others run meanwhile; Sleep(0) gives up the slice
            if (scheduler_) {
                if (a == 0)
                    scheduler_->yield();
                else
                    scheduler_->wait([] { return false; },
                                     a == 0xFFFFFFFFu
                                         ? std::nullopt
                                         : std::optional<std::uint64_t>(
                                               c.cycles + static_cast<std::uint64_t>(a) * 200000u));
                break;
            }
            if (a == 0xFFFFFFFFu)
                throw std::runtime_error("infinite Sleep needs a scheduler");
            c.cycles += static_cast<std::uint64_t>(a) * 200000u;
            break;
        case 6:
            result = 0xFFFFFFFFu;
            for (std::uint32_t slot = 0; slot < 64; ++slot)
                if (!tls_.contains(slot)) {
                    tls_[slot] = 0;
                    result = slot;
                    break;
                }
            if (result == 0xFFFFFFFFu)
                last_error_ = 8;
            break;
        case 7:
            result = static_cast<std::uint32_t>(tls_.erase(a));
            if (!result)
                last_error_ = 87;
            std::erase_if(tls_values_, [&](const auto& v) { return v.first.second == a; });
            if (result && a < 64 && process_)  // a freed slot reads 0 in every thread again
                for (const auto& [thread, block] : tls_blocks_) process_->write32(block + a * 4, 0);
            break;
        case 8:  // TlsGetValue: each thread has its own value per slot, 0 until set
            if (tls_.contains(a) && a < 64 && process_) {
                result = process_->read32(tls_block(thread_id()) + a * 4);
                last_error_ = 0;
            } else if (tls_.contains(a)) {
                const auto it = tls_values_.find({thread_id(), a});
                result = it == tls_values_.end() ? 0 : it->second;
                last_error_ = 0;
            } else {
                result = 0;
                last_error_ = 87;
            }
            break;
        case 9:
            if (tls_.contains(a) && a < 64 && process_) {
                process_->write32(tls_block(thread_id()) + a * 4, b);
                result = 1;
            } else if (tls_.contains(a)) {
                tls_values_[{thread_id(), a}] = b;
                result = 1;
            } else {
                result = 0;
                last_error_ = 87;
            }
            break;
        case 10:
            result = kProcessId;
            break;
        case 11:
            result = thread_id();
            break;
        case 12:
            throw ProcessExit{a};
        case 13:
        case 14:
        case 15:
        case 16:
        case 17: {
            result = 0;
            if (!modules_) {
                last_error_ = 120;
                break;
            }
            try {
                if (op == 13 || op == 14) {
                    // LoadLibraryEx flags requiring resources/altered search paths are not
                    // emulated.
                    if (op == 14 && (b || c.r[6])) {
                        last_error_ = 120;
                        break;
                    }
                    const auto h = modules_->load_library(basename(symbol_string(memory, a, true)));
                    modules_->initialize(entry_call_);
                    result = h;
                } else if (op == 15) {
                    result = a ? modules_->handle(basename(symbol_string(memory, a, true)))
                               : main_module_;
                    if (!result)
                        last_error_ = 126;
                } else {
                    Import symbol;
                    if (b <= 65535)
                        symbol.ordinal = static_cast<std::uint16_t>(b);
                    else
                        symbol.name = symbol_string(memory, b, op == 16);
                    result = modules_->procedure(a, symbol);
                    if (!result)
                        last_error_ = 127;
                }
            } catch (const PeError& e) {
                // A module a title asks for and cannot have is worth seeing: say which and why.
                std::fprintf(stderr, "wince: module call %u failed: %s\n", op, e.what());
                last_error_ = 126;
            } catch (const std::invalid_argument& e) {
                std::fprintf(stderr, "wince: module call %u failed: %s\n", op, e.what());
                last_error_ = 87;
            } catch (const std::runtime_error& e) {
                std::fprintf(stderr, "wince: module call %u failed: %s\n", op, e.what());
                last_error_ = 998;
            }
            break;
        }
        default:
            return false;
    }
    return true;
}
}  // namespace dream::wince
