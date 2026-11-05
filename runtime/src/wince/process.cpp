#include "dream/runtime/wince/process.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <string_view>

#include "dream/runtime/wince/kernel_services.h"
#include "dream/runtime/wince/modules.h"
#include "dream/runtime/wince/synchronization.h"

namespace dream::wince {
namespace {
void valid_range(std::uint32_t a, std::uint32_t n) {
    if (static_cast<std::uint64_t>(a) + n > 0x80000000ull)
        throw std::runtime_error("WinCE process address range overflow");
}
constexpr std::array<std::string_view, 23> kApis = {"GetTickCount",
                                                    "GetLastError",
                                                    "SetLastError",
                                                    "Sleep",
                                                    "LocalAlloc",
                                                    "LocalFree",
                                                    "TlsAlloc",
                                                    "TlsFree",
                                                    "TlsGetValue",
                                                    "TlsSetValue",
                                                    "GetCurrentProcessId",
                                                    "GetCurrentThreadId",
                                                    "ExitProcess",
                                                    "LoadLibraryW",
                                                    "LoadLibraryExW",
                                                    "GetModuleHandleW",
                                                    "GetProcAddressW",
                                                    "GetProcAddressA",
                                                    "CreateEventW",
                                                    "SetEvent",
                                                    "ResetEvent",
                                                    "WaitForSingleObject",
                                                    "CloseHandle"};
std::string api_name(std::string name) {
    if (name == "LoadLibrary" || name == "LoadLibraryEx" || name == "GetModuleHandle" ||
        name == "GetProcAddress" || name == "CreateEvent" || name == "CreateMutex")
        name += 'W';
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
    return candidate;
}
bool ProcessMemory::release(std::uint32_t base) {
    const auto it = regions_.find(base);
    if (it == regions_.end() || !it->second.allocation)
        return false;
    used_ -= static_cast<std::uint32_t>(it->second.bytes.size());
    --allocation_count_;
    regions_.erase(it);
    return true;
}
void ProcessMemory::check(std::uint32_t a, std::uint32_t size, unsigned permission) const {
    valid_range(a, size);
    while (size) {
        auto it = regions_.upper_bound(a);
        if (it == regions_.begin())
            throw std::runtime_error("unmapped WinCE process address");
        --it;
        const auto offset = a - it->first;
        const auto& r = it->second;
        if (offset >= r.bytes.size())
            throw std::runtime_error("unmapped WinCE process address");
        if ((r.permissions & permission) != permission)
            throw std::runtime_error("WinCE page protection violation");
        const auto n = std::min(size, static_cast<std::uint32_t>(r.bytes.size()) - offset);
        a += n;
        size -= n;
    }
}
std::vector<std::uint8_t> ProcessMemory::read_bytes(std::uint32_t a, std::uint32_t size) const {
    if (size > kBudget)
        throw std::runtime_error("WinCE read exceeds memory budget");
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
    check(a, static_cast<std::uint32_t>(bytes.size()), Write);
    for (std::size_t i = 0; i < bytes.size();) {
        const auto at = a + static_cast<std::uint32_t>(i);
        auto it = std::prev(regions_.upper_bound(at));
        const auto offset = at - it->first;
        const auto count = std::min(bytes.size() - i, it->second.bytes.size() - offset);
        std::copy_n(bytes.begin() + i, count, it->second.bytes.begin() + offset);
        i += count;
    }
}
std::uint64_t ProcessMemory::load(std::uint32_t a, unsigned size, unsigned permission) const {
    if (a & (std::min(size, 4u) - 1u))
        throw std::runtime_error("unaligned SH-4 process read");
    check(a, size, permission);
    std::uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) {
        const auto it = std::prev(regions_.upper_bound(a + i));
        value |= static_cast<std::uint64_t>(it->second.bytes[a + i - it->first]) << (8 * i);
    }
    return value;
}
void ProcessMemory::store(std::uint32_t a, std::uint64_t v, unsigned size) {
    if (a & (std::min(size, 4u) - 1u))
        throw std::runtime_error("unaligned SH-4 process write");
    std::array<std::uint8_t, 8> bytes{};
    for (unsigned i = 0; i < size; ++i) bytes[i] = static_cast<std::uint8_t>(v >> (8 * i));
    write_bytes(a, std::span<const std::uint8_t>(bytes.data(), size));
}
std::uint8_t ProcessMemory::read8(std::uint32_t a) {
    return static_cast<std::uint8_t>(load(a, 1));
}
std::uint16_t ProcessMemory::read16(std::uint32_t a) {
    return static_cast<std::uint16_t>(load(a, 2));
}
std::uint16_t ProcessMemory::fetch16(std::uint32_t a) {
    return static_cast<std::uint16_t>(load(a, 2, Execute));
}
std::uint32_t ProcessMemory::read32(std::uint32_t a) {
    return static_cast<std::uint32_t>(load(a, 4));
}
std::uint64_t ProcessMemory::read64(std::uint32_t a) {
    return load(a, 8);
}
void ProcessMemory::write8(std::uint32_t a, std::uint8_t v) {
    store(a, v, 1);
}
void ProcessMemory::write16(std::uint32_t a, std::uint16_t v) {
    store(a, v, 2);
}
void ProcessMemory::write32(std::uint32_t a, std::uint32_t v) {
    store(a, v, 4);
}
void ProcessMemory::write64(std::uint32_t a, std::uint64_t v) {
    store(a, v, 8);
}
void ProcessMemory::sq_write32(std::uint32_t, std::uint32_t) {
    throw std::runtime_error("store queues unavailable in HLE process");
}
void ProcessMemory::sq_flush(std::uint32_t) {
    throw std::runtime_error("store queues unavailable in HLE process");
}

std::optional<std::uint32_t> Kernel::resolve(const Import& imp) const {
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
    return KernelServices::resolve(name);
}
void Kernel::bind_ordinal(std::uint16_t ordinal, const std::string& name) {
    if (std::find(kApis.begin(), kApis.end(), api_name(name)) == kApis.end() &&
        !KernelServices::resolve(api_name(name)) && !Synchronization::resolve(api_name(name)))
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
Kernel::Kernel() = default;
Kernel::~Kernel() = default;
const std::vector<std::u16string>& Kernel::debug_messages() const {
    static const std::vector<std::u16string> empty;
    return services_ ? services_->debug_messages() : empty;
}
bool Kernel::invoke(sh4::Ctx& c, Memory& memory, std::uint32_t target) {
    // Preserve the original v2 event trap addresses while sharing typed handles with mutexes.
    if (target >= kTrapBase + 18 * 4 && target <= kTrapBase + 22 * 4 && !(target & 3u))
        target = *Synchronization::resolve(std::string(kApis[(target - kTrapBase) / 4]));
    if (Synchronization::handles(target)) {
        auto* process = dynamic_cast<ProcessMemory*>(&memory);
        if (!process)
            throw std::invalid_argument("COREDLL requires a WinCE process address space");
        if (!synchronization_)
            synchronization_ = std::make_unique<Synchronization>(*process);
        return synchronization_->invoke(c, memory, target, last_error_);
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
    const auto a = c.r[4], b = c.r[5];
    switch ((target - kTrapBase) / 4) {
        case 0:
            result = static_cast<std::uint32_t>(c.cycles / 200000u);
            break;
        case 1:
            result = last_error_;
            break;
        case 2:
            last_error_ = a;
            break;
        case 3:
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
            break;
        case 8:
            if (const auto it = tls_.find(a); it != tls_.end()) {
                result = it->second;
                last_error_ = 0;
            } else {
                result = 0;
                last_error_ = 87;
            }
            break;
        case 9:
            if (auto it = tls_.find(a); it != tls_.end()) {
                it->second = b;
                result = 1;
            } else {
                result = 0;
                last_error_ = 87;
            }
            break;
        case 10:
            result = 1;
            break;
        case 11:
            result = 2;
            break;
        case 12:
            throw ProcessExit{a};
        case 13:
        case 14:
        case 15:
        case 16:
        case 17: {
            const auto op = (target - kTrapBase) / 4;
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
                    const auto h = modules_->load_library(symbol_string(memory, a, true));
                    modules_->initialize(entry_call_);
                    result = h;
                } else if (op == 15) {
                    result = a ? modules_->handle(symbol_string(memory, a, true)) : main_module_;
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
            } catch (const PeError&) {
                last_error_ = 126;
            } catch (const std::invalid_argument&) {
                last_error_ = 87;
            } catch (const std::runtime_error&) {
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
