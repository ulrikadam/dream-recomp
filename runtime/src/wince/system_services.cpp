#include "dream/runtime/wince/system_services.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>

#include "dream/runtime/wince/abi.h"

namespace dream::wince {
namespace {
constexpr std::uint32_t kBase = 0x1F006000;
#define WINCE_SYSTEM(X)                                                                            \
    X(RegOpenKeyExW)                                                                               \
    X(RegCreateKeyExW)                                                                             \
    X(RegCloseKey)                                                                                 \
    X(RegQueryValueExW) X(RegSetValueExW) X(RegEnumKeyExW) X(RegQueryInfoKeyW) X(KernelIoControl)  \
        X(QueryAPISetID) X(CreateFileW) X(ReadFile) X(SetFilePointer) X(GetFileSize)               \
            X(DeviceIoControl) X(RegisterClassW) X(CreateWindowExW) X(DefWindowProcW) X(           \
                DestroyWindow) X(IsWindow) X(PeekMessageW) X(TranslateMessage) X(DispatchMessageW) \
                X(PostMessageW) X(PostQuitMessage) X(PostThreadMessageW) X(SetTimer) X(KillTimer)  \
                    X(GetKeyState) X(SetKMode) X(SetProcPermissions) X(EnumFontFamiliesW) X(       \
                        CreateFontIndirectW) X(CreateCompatibleDC) X(SelectObject) X(DeleteDC)     \
                        X(GetVersionExW) X(SetRect) X(SetRectEmpty) X(CopyRect) X(IsRectEmpty)     \
                            X(OffsetRect) X(InflateRect) X(IntersectRect) X(UnionRect)             \
                                X(EqualRect) X(VirtualCopy) X(HalAllocateCommonBufferWithId) X(    \
                                    HalAllocateCommonBuffer) X(HalFreeCommonBufferWithId)          \
                                    X(HalFreeCommonBuffer) X(SetStoreQueueBase) X(NKDbgPrintfW) X( \
                                        InterruptInitialize) X(InterruptDone) X(InterruptDisable)  \
                                        X(GetSystemMetrics) X(GetDeviceCaps) X(GetWindowLongW)     \
                                            X(SetWindowLongW) X(GetClassInfoW)                     \
                                                X(SetForegroundWindow) X(SetFocus)                 \
                                                    X(CreateBitmapFromPointer) X(DeleteObject)     \
                                                        X(MmProbeAndLockPages) X(MmUnlockPages)    \
                                                            X(HalMapInterrupt)
#define ENUM(name) name,
enum class Api { WINCE_SYSTEM(ENUM) Count };
#undef ENUM
#define NAME(name) #name,
constexpr std::array<std::string_view, static_cast<unsigned>(Api::Count)> kNames{
    WINCE_SYSTEM(NAME)};
#undef NAME
#undef WINCE_SYSTEM

constexpr std::uint32_t kSuccess = 0, kNotFound = 2, kInvalidHandle = 6, kInvalidParameter = 87,
                        kMoreData = 234, kNoMoreItems = 259;

std::u16string fold(std::u16string s) {
    for (auto& c : s)
        if (c >= u'A' && c <= u'Z')
            c = static_cast<char16_t>(c + 32);
    return s;
}
std::u16string read_wide(Memory& m, std::uint32_t p, unsigned limit = 1024) {
    if (!p || (p & 1u))
        throw std::invalid_argument("invalid UTF-16 pointer");
    std::u16string s;
    for (unsigned i = 0; i < limit; ++i) {
        const auto c = m.read16(p + i * 2);
        if (!c)
            return s;
        s += static_cast<char16_t>(c);
    }
    throw std::invalid_argument("unterminated UTF-16 string");
}
void write_wide(ProcessMemory& m, std::uint32_t p, const std::u16string& s) {
    if (p & 1u)
        throw std::invalid_argument("unaligned UTF-16 destination");
    std::vector<std::uint8_t> bytes((s.size() + 1) * 2);
    for (std::size_t i = 0; i < s.size(); ++i) {
        bytes[i * 2] = static_cast<std::uint8_t>(s[i]);
        bytes[i * 2 + 1] = static_cast<std::uint8_t>(s[i] >> 8);
    }
    m.write_bytes(p, bytes);
}
// "a\\\\b\\" -> "a\\b": no empty components, so joined paths compare as strings.
std::u16string clean(const std::u16string& path) {
    std::u16string out;
    for (const auto c : path) {
        if (c == u'\\' && (out.empty() || out.back() == u'\\'))
            continue;
        out += c;
    }
    while (!out.empty() && out.back() == u'\\') out.pop_back();
    return out;
}
std::optional<std::u16string> root_name(std::uint32_t handle) {
    switch (handle) {
        case 0x80000000u:
            return u"HKEY_CLASSES_ROOT";
        case 0x80000001u:
            return u"HKEY_CURRENT_USER";
        case 0x80000002u:
            return u"HKEY_LOCAL_MACHINE";
        case 0x80000003u:
            return u"HKEY_USERS";
        default:
            return {};
    }
}
bool is_root(const std::u16string& folded) {
    return folded.find(u'\\') == std::u16string::npos;
}
// For messages: ASCII as is, anything else as '?'.
std::string narrow(const std::u16string& s) {
    std::string out;
    for (const auto c : s) out += c < 128 ? static_cast<char>(c) : '?';
    return out;
}
std::u16string leaf(const std::u16string& path) {
    const auto at = path.rfind(u'\\');
    return at == std::u16string::npos ? path : path.substr(at + 1);
}
}  // namespace

constexpr std::uint32_t kBiosBase = kBase + 0x800;

SystemServices::SystemServices(ProcessMemory& memory)
    : flash_bytes_(hle::Flash::kSize, 0xFF), flash_(flash_bytes_.data()), memory_(memory) {
    flash_.format(hle::Language::French, hle::Region::Europe, hle::Broadcast::Pal);
}

std::optional<std::uint32_t> SystemServices::resolve(const std::string& name) {
    const auto it = std::find(kNames.begin(), kNames.end(), name);
    if (it == kNames.end())
        return {};
    return kBase + static_cast<std::uint32_t>(it - kNames.begin()) * 4;
}
bool SystemServices::handles(std::uint32_t target) {
    if (target >= kBiosBase && target < kBiosBase + kBiosVectors.size() * 4 && !(target & 3u))
        return true;
    return target >= kBase && target < kBase + kNames.size() * 4 && !(target & 3u);
}
std::uint32_t SystemServices::bios_stub(unsigned index) {
    return kBiosBase + index * 4;
}

// The BIOS syscalls, Katana convention: function number in R7 (R1 for the font), arguments in
// R4.., result in R0. Same behaviour as the Katana path (hle/bios.cpp) on this process's memory.
bool SystemServices::bios(sh4::Ctx& c, unsigned index) {
    auto& result = c.r[0];
    auto refuse = [&](std::uint32_t function) {
        const auto key = index << 16 | (function & 0xFFFF);
        if (std::find(refused_bios_.begin(), refused_bios_.end(), key) == refused_bios_.end()) {
            refused_bios_.push_back(key);
            std::fprintf(stderr, "wince: BIOS call vector 0x%08x function %u refused\n",
                         kBiosVectors[index], function);
        }
        result = 0xFFFFFFFFu;
    };
    switch (index) {
        case 0:  // SYSINFO (0x8C0000B0)
            if (c.r[7] == 0 || c.r[7] == 3) {
                if (!sysinfo_ && !(sysinfo_ = memory_.allocate(4096)))
                    throw std::runtime_error("no guest memory for the SYSINFO block");
                std::vector<std::uint8_t> data(24, 0);
                for (unsigned i = 0; i < 8; ++i) data[i] = flash_.read8(0x1A056 + i);
                for (unsigned i = 0; i < 5; ++i) data[8 + i] = flash_.read8(0x1A000 + i);
                memory_.write_bytes(sysinfo_, data);
                result = c.r[7] == 0 ? 0 : sysinfo_;
            } else {
                refuse(c.r[7]);
            }
            break;
        case 2:  // FLASHROM (0x8C0000B8)
            switch (c.r[7]) {
                case 0: {  // FLASHROM_INFO: r4 partition, r5 -> {offset, size}
                    std::uint32_t offset = 0, size = 0;
                    if (hle::Flash::partition(c.r[4], offset, size)) {
                        memory_.write32(c.r[5], offset);
                        memory_.write32(c.r[5] + 4, size);
                        result = 0;
                    } else {
                        result = 0xFFFFFFFFu;
                    }
                    break;
                }
                case 1: {  // FLASHROM_READ: r4 offset, r5 destination, r6 size
                    if (c.r[6] > hle::Flash::kSize) {
                        result = 0xFFFFFFFFu;
                        break;
                    }
                    std::vector<std::uint8_t> data(c.r[6]);
                    for (std::uint32_t i = 0; i < c.r[6]; ++i) data[i] = flash_.read8(c.r[4] + i);
                    memory_.write_bytes(c.r[5], data);
                    result = 0;
                    break;
                }
                case 2:  // FLASHROM_WRITE: kept in this run only
                    if (c.r[6] > hle::Flash::kSize) {
                        result = 0xFFFFFFFFu;
                        break;
                    }
                    for (std::uint32_t i = 0; i < c.r[6]; ++i)
                        flash_.program8(c.r[4] + i, memory_.read8(c.r[5] + i));
                    result = c.r[6];
                    break;
                default:
                    refuse(c.r[7]);
                    break;
            }
            break;
        case 1:  // ROMFONT (0x8C0000B4): function in R1
            refuse(c.r[1]);
            break;
        case 5: {  // MISC (0x8C0000E0): function in R4, as hle/bios.cpp sys_misc
            const auto key = index << 16 | (c.r[4] & 0xFFFF);
            if (std::find(refused_bios_.begin(), refused_bios_.end(), key) == refused_bios_.end()) {
                refused_bios_.push_back(key);
                std::fprintf(stderr, "wince: BIOS misc call %u (caller 0x%08x)%s\n", c.r[4], c.pr,
                             c.r[4] == 1 ? ": the title asks to go back to the console menu" : "");
            }
            result = c.r[4] == 0 ? 0x00C0BEBCu : 0;  // init; check disc (present); others 0
            break;
        }
        default:  // GD-ROM (0x8C0000BC/C0) and MISC (0x8C0000E0)
            refuse(c.r[7]);
            break;
    }
    return true;
}

void SystemServices::ensure_key(const std::u16string& path) {
    std::u16string prefix;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find(u'\\', start);
        const auto part = path.substr(start, end == std::u16string::npos ? end : end - start);
        prefix += (prefix.empty() ? u"" : u"\\") + part;
        auto& key = keys_[fold(prefix)];
        if (key.name.empty())
            key.name = part;
        if (end == std::u16string::npos)
            break;
        start = end + 1;
    }
}
void SystemServices::set_value(const std::u16string& key, const std::u16string& name,
                               std::uint32_t type, std::vector<std::uint8_t> data) {
    const auto path = clean(key);
    ensure_key(path);
    keys_[fold(path)].values[fold(name)] = Value{name, type, std::move(data)};
}
bool SystemServices::has_key(const std::u16string& key) const {
    return keys_.contains(fold(clean(key)));
}
std::optional<std::filesystem::path> SystemServices::host_path(const std::u16string& name) const {
    if (root_.empty())
        return {};
    std::vector<std::u16string> parts;
    std::u16string part;
    for (const auto c : name + u"\\") {
        if (c == u'\\' || c == u'/') {
            if (!part.empty())
                parts.push_back(part);
            part.clear();
        } else if (c == u':' || c < 32) {
            return {};
        } else {
            part += c;
        }
    }
    if (!parts.empty() && fold(parts.front()) == u"cd-rom")
        parts.erase(parts.begin());
    if (parts.empty())
        return {};
    auto path = root_;
    for (const auto& p : parts) {
        if (p == u"." || p == u"..")
            return {};
        path /= std::filesystem::path(p);
    }
    return path;
}

std::optional<std::u16string> SystemServices::key_path(std::uint32_t handle) const {
    if (const auto root = root_name(handle))
        return fold(*root);
    const auto it = open_.find(handle);
    if (it == open_.end())
        return {};
    return it->second;
}

// "hkey_classes_root\clsid\{guid}[\inprocserver32]" missing: ask the resolver once per CLSID.
bool SystemServices::discover_class(const std::u16string& path) {
    static const std::u16string prefix = u"hkey_classes_root\\clsid\\{";
    if (!class_resolver_ || !path.starts_with(prefix))
        return false;
    const auto close = path.find(u'}', prefix.size());
    if (close != prefix.size() + 36)
        return false;
    const auto text = path.substr(prefix.size(), 36);
    if (const auto it = classes_asked_.find(text); it != classes_asked_.end())
        return false;  // asked before: either recorded already, or no DLL serves it
    const auto rest = path.substr(close + 1);
    if (!rest.empty() && rest != u"\\inprocserver32")
        return false;
    // {aabbccdd-eeff-gghh-iijj-kkllmmnnoopp} -> GUID memory layout (first three fields LE).
    std::string hex;
    for (const auto ch : text)
        if (ch != u'-')
            hex += static_cast<char>(ch);
    if (hex.size() != 32)
        return false;
    std::array<std::uint8_t, 16> raw{};
    for (unsigned i = 0; i < 16; ++i)
        raw[i] = static_cast<std::uint8_t>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    std::array<std::uint8_t, 16> clsid = raw;
    std::swap(clsid[0], clsid[3]);
    std::swap(clsid[1], clsid[2]);
    std::swap(clsid[4], clsid[5]);
    std::swap(clsid[6], clsid[7]);
    const auto dll = class_resolver_(clsid);
    classes_asked_[text] = dll.has_value();
    if (!dll)
        return false;
    std::vector<std::uint8_t> value;
    for (const auto ch : *dll) {
        value.push_back(static_cast<std::uint8_t>(ch));
        value.push_back(0);
    }
    value.push_back(0);
    value.push_back(0);
    const std::u16string key = u"HKEY_CLASSES_ROOT\\CLSID\\{" + text + u"}\\InprocServer32";
    set_value(key, u"", 1 /* REG_SZ */, value);
    std::fprintf(stderr, "wince: COM class {%s} served by %s\n", narrow(text).c_str(),
                 dll->c_str());
    return keys_.contains(path);
}

namespace {
// SYSINTR -> the Holly status bits that raise it, read from the OAL's handler and its masks
// (nk.exe 0x8C02E000-0x8C02E1BC). Its table (0x8C145EA8, one 24-byte entry per SYSINTR 16-27)
// also holds each group's enabled bits and the status last acknowledged.
struct Route {
    std::uint32_t sysintr, nrm, ext, err;
};
constexpr Route kRoutes[] = {
    {16, 0x00080FFFu, 0, 0},  // PowerVR: renders, blanking, TA lists, PVR and ch2 DMA
    {17, 0, 0, 0x000000FFu},  // PowerVR/TA errors
    {18, 0x00003000u, 0, 0},  // Maple DMA and Maple V-blank
    {19, 0, 0, 0x00000F00u},  // Maple errors
    {20, 0, 0x1u, 0},         // GD-ROM
    {21, 0x00004000u, 0, 0},  // GD-ROM DMA
    {23, 0, 0x2u, 0},         // AICA (the ARM7 signalling the SH-4)
    {24, 0x00008000u, 0, 0},  // AICA DMA
    {25, 0, 0, 0x00888000u},  // other errors
    {26, 0, 0x4u, 0},         // modem
    {27, 0, 0x8u, 0},         // external device
};
const Route* route_of(std::uint32_t sysintr) {
    for (const auto& r : kRoutes)
        if (r.sysintr == sysintr)
            return &r;
    return nullptr;
}
}  // namespace

std::vector<std::uint32_t> SystemServices::due_interrupts(std::uint32_t istnrm,
                                                          std::uint32_t istext,
                                                          std::uint32_t isterr) {
    std::vector<std::uint32_t> events;
    for (const auto& route : kRoutes) {
        const auto enabled = enabled_of(route.sysintr);
        if (!(istnrm & route.nrm & enabled) && !(istext & route.ext & enabled) &&
            !(isterr & route.err & enabled))
            continue;
        const auto it = interrupts_.find(route.sysintr);
        if (it == interrupts_.end() || !it->second.armed)
            continue;
        it->second.armed = false;
        if (it->second.fired++ == 0)  // the totals are in interrupt_counts()
            std::fprintf(stderr,
                         "wince: SYSINTR %u fires first: ISTNRM %08x ISTEXT %08x ISTERR %08x\n",
                         route.sysintr, istnrm, istext, isterr);
        events.push_back(it->second.event);
    }
    return events;
}

std::uint32_t SystemServices::enabled_of(std::uint32_t sysintr) {
    const auto it = enabled_.find(sysintr);
    if (it != enabled_.end())
        return it->second;
    const auto* route = route_of(sysintr);
    return route ? route->nrm | route->ext | route->err : 0u;  // everything, until restricted
}

std::uint32_t SystemServices::send(std::uint32_t window, std::uint32_t message,
                                   std::uint32_t wparam, std::uint32_t lparam) {
    const auto it = windows_.find(window);
    if (it == windows_.end() || !it->second.procedure)
        return 0;  // DefWindowProc
    if (!guest_call_)
        throw std::runtime_error("window procedures need guest calls (the interpreter)");
    return guest_call_(it->second.procedure, {window, message, wparam, lparam});
}

bool SystemServices::invoke(sh4::Ctx& c, Memory& memory, std::uint32_t target,
                            std::uint32_t& last_error) {
    if (!handles(target))
        return false;
    if (&memory != &memory_)
        throw std::runtime_error("COREDLL system services used with another process");
    if (target >= kBiosBase)
        return bios(c, (target - kBiosBase) / 4);
    auto arg = [&](unsigned i) { return argument(c, memory, i); };
    auto put32 = [&](std::uint32_t p, std::uint32_t v) {
        if (p)
            memory.write32(p, v);
    };
    auto& result = c.r[0];
    switch (static_cast<Api>((target - kBase) / 4)) {
        case Api::RegOpenKeyExW: {
            const auto base = key_path(arg(0));
            const auto out = arg(4);
            if (!base || !out) {
                result = base ? kInvalidParameter : kInvalidHandle;
                break;
            }
            const auto sub = arg(1) ? clean(read_wide(memory, arg(1))) : std::u16string();
            const auto path = sub.empty() ? *base : *base + u"\\" + fold(sub);
            if (!is_root(path) && !keys_.contains(path) && !discover_class(path)) {
                std::fprintf(stderr, "wince: registry key \"%s\" not found\n",
                             narrow(path).c_str());
                result = kNotFound;
                break;
            }
            const auto handle = next_handle_;
            next_handle_ += 4;
            open_[handle] = path;
            put32(out, handle);
            result = kSuccess;
            break;
        }
        case Api::RegCreateKeyExW: {
            // (hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired, lpSecurityAttributes,
            //  phkResult, lpdwDisposition)
            const auto base = key_path(arg(0));
            const auto out = arg(7);
            if (!base || !out || !arg(1)) {
                result = base ? kInvalidParameter : kInvalidHandle;
                break;
            }
            const auto sub = clean(read_wide(memory, arg(1)));
            if (sub.empty()) {
                result = kInvalidParameter;
                break;
            }
            // New components keep the case the title wrote (enumeration shows it).
            const auto existed = keys_.contains(*base + u"\\" + fold(sub));
            ensure_key(*base + u"\\" + sub);
            const auto path = *base + u"\\" + fold(sub);
            const auto handle = next_handle_;
            next_handle_ += 4;
            open_[handle] = path;
            put32(out, handle);
            put32(arg(8), existed ? 2u : 1u);  // REG_OPENED_EXISTING_KEY / REG_CREATED_NEW_KEY
            result = kSuccess;
            break;
        }
        case Api::RegCloseKey:
            result = root_name(arg(0)) || open_.erase(arg(0)) ? kSuccess : kInvalidHandle;
            break;
        case Api::RegQueryValueExW: {
            // (hKey, lpValueName, lpReserved, lpType, lpData, lpcbData)
            const auto path = key_path(arg(0));
            if (!path) {
                result = kInvalidHandle;
                break;
            }
            const auto key = keys_.find(*path);
            const auto name = arg(1) ? fold(read_wide(memory, arg(1))) : std::u16string();
            if (key == keys_.end() || !key->second.values.contains(name)) {
                result = kNotFound;
                break;
            }
            const auto& value = key->second.values.at(name);
            const auto size = static_cast<std::uint32_t>(value.data.size());
            put32(arg(3), value.type);
            const auto data = arg(4), size_ptr = arg(5);
            result = kSuccess;
            if (data) {
                if (!size_ptr) {
                    result = kInvalidParameter;
                    break;
                }
                if (memory.read32(size_ptr) < size)
                    result = kMoreData;
                else if (size)
                    memory_.write_bytes(data, value.data);
            }
            put32(size_ptr, size);
            break;
        }
        case Api::RegSetValueExW: {
            // (hKey, lpValueName, Reserved, dwType, lpData, cbData)
            const auto path = key_path(arg(0));
            if (!path || is_root(*path)) {
                result = path ? kInvalidParameter : kInvalidHandle;
                break;
            }
            const auto name = arg(1) ? read_wide(memory, arg(1)) : std::u16string();
            const auto size = arg(5);
            if (size > 65536 || (size && !arg(4))) {
                result = kInvalidParameter;
                break;
            }
            keys_[*path].values[fold(name)] =
                Value{name, arg(3),
                      size ? memory_.read_bytes(arg(4), size) : std::vector<std::uint8_t>{}};
            result = kSuccess;
            break;
        }
        case Api::RegEnumKeyExW: {
            // (hKey, dwIndex, lpName, lpcName, lpReserved, lpClass, lpcClass, lpftLastWriteTime)
            const auto path = key_path(arg(0));
            if (!path) {
                result = kInvalidHandle;
                break;
            }
            std::vector<std::u16string> children;
            const auto prefix = *path + u"\\";
            for (const auto& [full, key] : keys_)
                if (full.starts_with(prefix) && full.find(u'\\', prefix.size()) == full.npos)
                    children.push_back(key.name);
            const auto index = arg(1), name = arg(2), length = arg(3);
            if (index >= children.size()) {
                result = kNoMoreItems;
                break;
            }
            if (!name || !length) {
                result = kInvalidParameter;
                break;
            }
            const auto& child = children[index];
            if (memory.read32(length) <= child.size()) {
                put32(length, static_cast<std::uint32_t>(child.size() + 1));
                result = kMoreData;
                break;
            }
            write_wide(memory_, name, child);
            put32(length, static_cast<std::uint32_t>(child.size()));
            if (arg(6))
                put32(arg(6), 0);
            if (arg(7)) {
                put32(arg(7), 0);
                put32(arg(7) + 4, 0);
            }
            result = kSuccess;
            break;
        }
        case Api::RegQueryInfoKeyW: {
            // (hKey, lpClass, lpcClass, lpReserved, lpcSubKeys, lpcMaxSubKeyLen, lpcMaxClassLen,
            //  lpcValues, lpcMaxValueNameLen, lpcMaxValueLen, lpcbSecurityDescriptor,
            //  lpftLastWriteTime)
            const auto path = key_path(arg(0));
            if (!path) {
                result = kInvalidHandle;
                break;
            }
            std::uint32_t subkeys = 0, longest_key = 0, values = 0, longest_name = 0,
                          longest_value = 0;
            const auto prefix = *path + u"\\";
            for (const auto& [full, key] : keys_)
                if (full.starts_with(prefix) && full.find(u'\\', prefix.size()) == full.npos) {
                    ++subkeys;
                    longest_key =
                        std::max(longest_key, static_cast<std::uint32_t>(key.name.size()));
                }
            if (const auto it = keys_.find(*path); it != keys_.end())
                for (const auto& [folded, value] : it->second.values) {
                    ++values;
                    longest_name =
                        std::max(longest_name, static_cast<std::uint32_t>(value.name.size()));
                    longest_value =
                        std::max(longest_value, static_cast<std::uint32_t>(value.data.size()));
                }
            if (arg(1) && arg(2)) {
                if (memory.read32(arg(2)) < 1) {
                    result = kMoreData;
                    break;
                }
                memory.write16(arg(1), 0);
            }
            put32(arg(2), 0);
            put32(arg(4), subkeys);
            put32(arg(5), longest_key);
            put32(arg(6), 0);
            put32(arg(7), values);
            put32(arg(8), longest_name);
            put32(arg(9), longest_value);
            put32(arg(10), 0);
            if (arg(11)) {
                put32(arg(11), 0);
                put32(arg(11) + 4, 0);
            }
            result = kSuccess;
            break;
        }
        case Api::KernelIoControl: {
            // (dwIoControlCode, lpInBuf, nInBufSize, lpOutBuf, nOutBufSize, lpBytesReturned).
            // The codes are the OEM adaptation's. Those not handled below are refused the way a
            // kernel refuses a code it does not handle, each new one reported once.
            const auto code = arg(0);
            // The Dreamcast OAL's interrupt codes (nk.exe OEMIoControl, 0x8C02DC30), each taking
            // the SYSINTR first: 0x10000 returns the group's pending status bits and acknowledges
            // them in Holly (kept until InterruptDone, as the OAL caches them); 0x20000 and
            // 0x30000 enable and disable status bits of the group.
            if ((code == 0x10000 || code == 0x20000 || code == 0x30000) && arg(1) && arg(2) >= 4) {
                const auto sysintr = memory.read32(arg(1));
                const auto* route = route_of(sysintr);
                if (!route) {
                    last_error = kInvalidParameter;
                    result = 0;
                    break;
                }
                const auto group = route->nrm | route->ext | route->err;
                if (code == 0x10000) {
                    auto& cached = acknowledged_[sysintr];
                    if (!cached) {
                        const auto reg = route->nrm   ? 0xA05F6900u
                                         : route->ext ? 0xA05F6904u
                                                      : 0xA05F6908u;
                        cached = memory.read32(reg) & group;
                        if (cached && !route->ext)  // external sources are cleared at the device
                            memory.write32(reg, cached);
                    }
                    put32(arg(3), cached);
                    put32(arg(5), 4);
                } else {
                    const auto bits = (arg(2) >= 8 ? memory.read32(arg(1) + 4) : 0u) & group;
                    auto enabled = enabled_of(sysintr);
                    enabled = code == 0x20000 ? enabled | bits : enabled & ~bits;
                    enabled_[sysintr] = enabled;
                }
                result = 1;
                break;
            }
            if (std::find(refused_ioctls_.begin(), refused_ioctls_.end(), code) ==
                refused_ioctls_.end()) {
                refused_ioctls_.push_back(code);
                std::fprintf(stderr,
                             "wince: KernelIoControl(0x%x, in %u bytes, out %u bytes) refused\n",
                             code, arg(2), arg(4));
            }
            put32(arg(5), 0);
            last_error = 50;  // ERROR_NOT_SUPPORTED
            result = 0;
            break;
        }
        case Api::QueryAPISetID: {
            // (char* name): the index of an API set another process registered, or -1. No
            // system process runs here, so none is registered. Each name is reported once.
            std::string name;
            for (std::uint32_t p = arg(0); p && name.size() < 16; ++p) {
                const auto ch = memory.read8(p);
                if (!ch)
                    break;
                name += static_cast<char>(ch);
            }
            if (std::find(missing_api_sets_.begin(), missing_api_sets_.end(), name) ==
                missing_api_sets_.end()) {
                missing_api_sets_.push_back(name);
                std::fprintf(stderr, "wince: API set \"%s\" not registered\n", name.c_str());
            }
            result = 0xFFFFFFFFu;
            break;
        }
        case Api::CreateFileW: {
            // (lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
            //  dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile)
            constexpr std::uint32_t kInvalid = 0xFFFFFFFFu, kGenericWrite = 0x40000000u;
            const auto name = read_wide(memory, arg(0));
            const auto shown = narrow(name);
            result = kInvalid;
            if (fold(name).starts_with(u"\\device\\")) {
                const auto handle = next_file_;
                next_file_ += 4;
                files_[handle] = File{name, true, nullptr, 0, 0};
                // Drivers are opened again and again (input polling): each is reported once.
                if (std::find(devices_opened_.begin(), devices_opened_.end(), fold(name)) ==
                    devices_opened_.end()) {
                    devices_opened_.push_back(fold(name));
                    std::fprintf(stderr, "wince: open device \"%s\"\n", shown.c_str());
                }
                result = handle;
                break;
            }
            const auto disposition = arg(4);
            if ((arg(1) & kGenericWrite) || (disposition != 3 && disposition != 4)) {
                std::fprintf(stderr, "wince: write access to \"%s\" refused\n", shown.c_str());
                last_error = 5;  // ERROR_ACCESS_DENIED: the disc is read only
                break;
            }
            const auto path = host_path(name);
            std::error_code ec;
            if (!path || !std::filesystem::is_regular_file(*path, ec)) {
                std::fprintf(stderr, "wince: open \"%s\": not found\n", shown.c_str());
                last_error = 2;  // ERROR_FILE_NOT_FOUND
                break;
            }
            auto stream = std::make_unique<std::ifstream>(*path, std::ios::binary);
            if (!*stream) {
                last_error = 5;
                break;
            }
            const auto size = std::filesystem::file_size(*path, ec);
            const auto handle = next_file_;
            next_file_ += 4;
            files_[handle] = File{name, false, std::move(stream), ec ? 0 : size, 0};
            std::fprintf(stderr, "wince: open \"%s\" (%llu bytes)\n", shown.c_str(),
                         static_cast<unsigned long long>(files_[handle].size));
            result = handle;
            break;
        }
        case Api::ReadFile: {
            // (hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped)
            const auto it = files_.find(arg(0));
            result = 0;
            put32(arg(3), 0);
            if (it == files_.end()) {
                last_error = kInvalidHandle;
                break;
            }
            auto& file = it->second;
            if (file.device || arg(4)) {
                last_error = 1;  // ERROR_INVALID_FUNCTION: no sector reads, no overlapped I/O
                break;
            }
            const auto want =
                std::min<std::uint64_t>(arg(2), file.size - std::min(file.size, file.position));
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(want));
            file.stream->clear();
            file.stream->seekg(static_cast<std::streamoff>(file.position));
            file.stream->read(reinterpret_cast<char*>(bytes.data()),
                              static_cast<std::streamsize>(bytes.size()));
            bytes.resize(static_cast<std::size_t>(file.stream->gcount()));
            if (!bytes.empty())
                memory_.write_bytes(arg(1), bytes);
            file.position += bytes.size();
            put32(arg(3), static_cast<std::uint32_t>(bytes.size()));
            result = 1;
            break;
        }
        case Api::SetFilePointer: {
            // (hFile, lDistanceToMove, lpDistanceToMoveHigh, dwMoveMethod)
            const auto it = files_.find(arg(0));
            result = 0xFFFFFFFFu;
            if (it == files_.end() || it->second.device) {
                last_error = kInvalidHandle;
                break;
            }
            auto& file = it->second;
            std::int64_t distance = static_cast<std::int32_t>(arg(1));
            if (arg(2))
                distance = static_cast<std::int64_t>(
                    (static_cast<std::uint64_t>(memory.read32(arg(2))) << 32) | arg(1));
            const std::int64_t from = arg(3) == 0   ? 0
                                      : arg(3) == 1 ? static_cast<std::int64_t>(file.position)
                                      : arg(3) == 2 ? static_cast<std::int64_t>(file.size)
                                                    : -1;
            if (from < 0) {
                last_error = kInvalidParameter;
                break;
            }
            if (from + distance < 0) {
                last_error = 131;  // ERROR_NEGATIVE_SEEK
                break;
            }
            file.position = static_cast<std::uint64_t>(from + distance);
            put32(arg(2), static_cast<std::uint32_t>(file.position >> 32));
            result = static_cast<std::uint32_t>(file.position);
            break;
        }
        case Api::GetFileSize: {
            // (hFile, lpFileSizeHigh)
            const auto it = files_.find(arg(0));
            if (it == files_.end() || it->second.device) {
                last_error = kInvalidHandle;
                result = 0xFFFFFFFFu;
                break;
            }
            put32(arg(1), static_cast<std::uint32_t>(it->second.size >> 32));
            result = static_cast<std::uint32_t>(it->second.size);
            break;
        }
        case Api::DeviceIoControl: {
            // (hDevice, dwIoControlCode, lpInBuffer, nInBufferSize, lpOutBuffer, nOutBufferSize,
            //  lpBytesReturned, lpOverlapped). No device command is emulated yet: each code is
            // refused and reported once, so the ones a title depends on show up.
            const auto it = files_.find(arg(0));
            const auto code = arg(1);
            put32(arg(6), 0);
            result = 0;
            if (it == files_.end()) {
                last_error = kInvalidHandle;
                break;
            }
            if (std::find(refused_device_ioctls_.begin(), refused_device_ioctls_.end(), code) ==
                refused_device_ioctls_.end()) {
                refused_device_ioctls_.push_back(code);
                const auto device = narrow(it->second.name);
                std::fprintf(stderr,
                             "wince: DeviceIoControl(\"%s\", 0x%08x, in %u bytes, out %u bytes) "
                             "refused\n",
                             device.c_str(), code, arg(3), arg(5));
            }
            last_error = 50;  // ERROR_NOT_SUPPORTED
            break;
        }
        case Api::RegisterClassW: {
            // WNDCLASSW: style, lpfnWndProc, cbClsExtra, cbWndExtra, hInstance, hIcon, hCursor,
            // hbrBackground, lpszMenuName, lpszClassName.
            const auto wc = arg(0);
            const auto name_ptr = memory.read32(wc + 36);
            if (name_ptr < 0x10000) {
                last_error = kInvalidParameter;  // class atoms are not supported
                result = 0;
                break;
            }
            const auto name = read_wide(memory, name_ptr);
            classes_[fold(name)] =
                WindowClass{memory.read32(wc + 4), memory.read32(wc), memory.read32(wc + 16)};
            std::fprintf(stderr, "wince: window class \"%s\" registered\n", narrow(name).c_str());
            result = 0xC000u + static_cast<std::uint32_t>(classes_.size());
            break;
        }
        case Api::CreateWindowExW: {
            // (dwExStyle, lpClassName, lpWindowName, dwStyle, x, y, nWidth, nHeight,
            //  hWndParent, hMenu, hInstance, lpParam)
            const auto class_name =
                arg(1) >= 0x10000 ? read_wide(memory, arg(1)) : std::u16string(u"#atom");
            const auto it = classes_.find(fold(class_name));
            if (it == classes_.end())
                std::fprintf(stderr,
                             "wince: window class \"%s\" was never registered; the window gets "
                             "no procedure\n",
                             narrow(class_name).c_str());
            const auto window = next_window_;
            next_window_ += 4;
            windows_[window] =
                Window{class_name, it == classes_.end() ? 0u : it->second.procedure, arg(3)};
            if (!scratch_ && !(scratch_ = memory_.allocate(4096)))
                throw std::runtime_error("no guest memory for window structures");
            // CREATESTRUCTW: lpCreateParams, hInstance, hMenu, hwndParent, cy, cx, y, x, style,
            // lpszName, lpszClass, dwExStyle.
            const std::array<std::uint32_t, 12> create{arg(11), arg(10), arg(9), arg(8),
                                                       arg(7),  arg(6),  arg(5), arg(4),
                                                       arg(3),  arg(2),  arg(1), arg(0)};
            for (unsigned i = 0; i < create.size(); ++i)
                memory.write32(scratch_ + i * 4, create[i]);
            std::fprintf(stderr, "wince: window 0x%08x created (%ux%u)\n", window, arg(6), arg(7));
            if (send(window, 0x0001 /* WM_CREATE */, 0, scratch_) == 0xFFFFFFFFu) {
                windows_.erase(window);
                last_error = 1407;  // ERROR_CANNOT_FIND_WND_CLASS-like failure from WM_CREATE
                result = 0;
                break;
            }
            result = window;
            break;
        }
        case Api::DefWindowProcW:
            result = 0;
            break;
        case Api::DestroyWindow: {
            const auto window = arg(0);
            if (!windows_.contains(window)) {
                last_error = 1400;  // ERROR_INVALID_WINDOW_HANDLE
                result = 0;
                break;
            }
            send(window, 0x0002 /* WM_DESTROY */, 0, 0);
            windows_.erase(window);
            std::erase_if(timers_, [&](const Timer& t) { return t.window == window; });
            result = 1;
            break;
        }
        case Api::IsWindow:
            result = windows_.contains(arg(0)) ? 1 : 0;
            break;
        case Api::PeekMessageW: {
            // (lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg). MSG: hwnd, message,
            // wParam, lParam, time, pt.x, pt.y.
            const auto out = arg(0), window = arg(1), low = arg(2), high = arg(3);
            const bool remove = (arg(4) & 1u) != 0;
            const auto now = c.cycles / 200000u;
            auto wanted = [&](std::uint32_t w, std::uint32_t m) {
                return (!window || w == window) && ((!low && !high) || (m >= low && m <= high));
            };
            std::optional<Message> found;
            if (const auto it =
                    std::find_if(posted_.begin(), posted_.end(),
                                 [&](const Message& m) { return wanted(m.window, m.message); });
                it != posted_.end()) {
                found = *it;
                if (remove)
                    posted_.erase(it);
            } else if (quit_ && wanted(0, 0x0012)) {
                found = Message{0, 0x0012 /* WM_QUIT */, quit_code_, 0};
                if (remove)
                    quit_ = false;
            } else {
                for (auto& t : timers_)
                    if (t.due <= now && wanted(t.window, 0x0113)) {
                        found = Message{t.window, 0x0113 /* WM_TIMER */, t.id, t.callback};
                        if (remove)
                            t.due = now + std::max<std::uint32_t>(t.period, 1);
                        break;
                    }
            }
            result = found ? 1 : 0;
            if (found) {
                const std::array<std::uint32_t, 7> msg{found->window,
                                                       found->message,
                                                       found->wparam,
                                                       found->lparam,
                                                       static_cast<std::uint32_t>(now),
                                                       0,
                                                       0};
                for (unsigned i = 0; i < msg.size(); ++i) memory.write32(out + i * 4, msg[i]);
            }
            break;
        }
        case Api::TranslateMessage:
            result = 0;
            break;
        case Api::DispatchMessageW: {
            const auto msg = arg(0);
            const auto window = memory.read32(msg), message = memory.read32(msg + 4),
                       wparam = memory.read32(msg + 8), lparam = memory.read32(msg + 12);
            if (message == 0x0113 && lparam) {  // WM_TIMER with a callback
                if (!guest_call_)
                    throw std::runtime_error("timer callbacks need guest calls (the interpreter)");
                result = guest_call_(lparam, {window, message, wparam, memory.read32(msg + 16)});
            } else {
                result = send(window, message, wparam, lparam);
            }
            break;
        }
        case Api::PostMessageW:
        case Api::PostThreadMessageW: {
            const bool to_window = static_cast<Api>((target - kBase) / 4) == Api::PostMessageW;
            if (to_window && arg(0) && !windows_.contains(arg(0))) {
                last_error = 1400;
                result = 0;
                break;
            }
            if (posted_.size() >= 10000) {
                last_error = 1816;  // ERROR_NOT_ENOUGH_QUOTA
                result = 0;
                break;
            }
            posted_.push_back(Message{to_window ? arg(0) : 0, arg(1), arg(2), arg(3)});
            result = 1;
            break;
        }
        case Api::PostQuitMessage:
            quit_ = true;
            quit_code_ = arg(0);
            break;
        case Api::SetTimer: {
            // (hWnd, nIDEvent, uElapse, lpTimerFunc)
            const auto window = arg(0);
            if (window && !windows_.contains(window)) {
                last_error = 1400;
                result = 0;
                break;
            }
            const auto id = window ? arg(1) : next_timer_++;
            std::erase_if(timers_,
                          [&](const Timer& t) { return t.window == window && t.id == id; });
            timers_.push_back(Timer{window, id, arg(2), arg(3), c.cycles / 200000u + arg(2)});
            result = id ? id : 1;
            break;
        }
        case Api::KillTimer: {
            const auto before = timers_.size();
            std::erase_if(timers_,
                          [&](const Timer& t) { return t.window == arg(0) && t.id == arg(1); });
            result = timers_.size() != before ? 1 : 0;
            break;
        }
        case Api::GetKeyState:
            result = 0;  // no keyboard: every key up
            break;
        // Both return the previous setting. The process already sees everything it maps; what
        // kernel mode really unlocks (direct hardware addresses) shows up as unmapped accesses.
        case Api::SetKMode:
            result = kernel_mode_ ? 1 : 0;
            kernel_mode_ = arg(0) != 0;
            break;
        case Api::SetProcPermissions:
            result = permissions_;
            permissions_ = arg(0);
            break;
        // GDI placeholders: no font is installed and nothing is drawn. Device contexts and
        // fonts are handles with no state, so a title that only prepares them carries on; one
        // that really draws text through GDI will need a renderer here.
        case Api::EnumFontFamiliesW: {
            // (hdc, lpszFamily, lpEnumFontFamProc, lParam): no family matches, so the callback
            // never runs and the call reports success.
            const auto family = arg(1) ? narrow(read_wide(memory, arg(1))) : std::string("(all)");
            std::fprintf(stderr, "wince: font family \"%s\" requested: none installed\n",
                         family.c_str());
            result = 1;
            break;
        }
        case Api::CreateFontIndirectW:
        case Api::CreateCompatibleDC: {
            const auto handle = next_gdi_;
            next_gdi_ += 4;
            gdi_.push_back(handle);
            result = handle;
            break;
        }
        case Api::SelectObject:
            // (hdc, hgdiobj): the previous object, which is "none" for a fresh context.
            result = std::find(gdi_.begin(), gdi_.end(), arg(0)) != gdi_.end() ? 0x1D7FFFF0u : 0;
            break;
        case Api::GetVersionExW: {
            // OSVERSIONINFOW: size, major, minor, build, platform, szCSDVersion[128]. The
            // Dreamcast's COREDLL is built for subsystem 2.11: Windows CE 2.11.
            const auto info = arg(0);
            if (!info || memory.read32(info) < 276) {
                last_error = kInvalidParameter;
                result = 0;
                break;
            }
            std::vector<std::uint8_t> bytes(276, 0);
            const std::array<std::uint32_t, 5> fields{276, 2, 11, 0, 3 /* VER_PLATFORM_WIN32_CE */};
            for (unsigned i = 0; i < fields.size(); ++i)
                for (unsigned b = 0; b < 4; ++b)
                    bytes[i * 4 + b] = static_cast<std::uint8_t>(fields[i] >> (8 * b));
            memory_.write_bytes(info, bytes);
            result = 1;
            break;
        }
        // RECT helpers: left, top, right, bottom as signed 32-bit, as user32 has them.
        case Api::SetRect:
        case Api::SetRectEmpty:
        case Api::CopyRect:
        case Api::IsRectEmpty:
        case Api::OffsetRect:
        case Api::InflateRect:
        case Api::IntersectRect:
        case Api::UnionRect:
        case Api::EqualRect: {
            using Rect = std::array<std::int32_t, 4>;
            auto load = [&](std::uint32_t p) {
                Rect r{};
                for (unsigned i = 0; i < 4; ++i)
                    r[i] = static_cast<std::int32_t>(memory.read32(p + i * 4));
                return r;
            };
            auto store = [&](std::uint32_t p, const Rect& r) {
                for (unsigned i = 0; i < 4; ++i)
                    memory.write32(p + i * 4, static_cast<std::uint32_t>(r[i]));
            };
            auto empty = [](const Rect& r) { return r[2] <= r[0] || r[3] <= r[1]; };
            const auto api = static_cast<Api>((target - kBase) / 4);
            if (!arg(0)) {
                result = 0;
                break;
            }
            result = 1;
            switch (api) {
                case Api::SetRect:
                    store(arg(0),
                          {static_cast<std::int32_t>(arg(1)), static_cast<std::int32_t>(arg(2)),
                           static_cast<std::int32_t>(arg(3)), static_cast<std::int32_t>(arg(4))});
                    break;
                case Api::SetRectEmpty:
                    store(arg(0), {});
                    break;
                case Api::CopyRect:
                    store(arg(0), load(arg(1)));
                    break;
                case Api::IsRectEmpty:
                    result = empty(load(arg(0))) ? 1 : 0;
                    break;
                case Api::OffsetRect:
                case Api::InflateRect: {
                    auto r = load(arg(0));
                    const auto dx = static_cast<std::int32_t>(arg(1)),
                               dy = static_cast<std::int32_t>(arg(2));
                    if (api == Api::OffsetRect)
                        r = {r[0] + dx, r[1] + dy, r[2] + dx, r[3] + dy};
                    else
                        r = {r[0] - dx, r[1] - dy, r[2] + dx, r[3] + dy};
                    store(arg(0), r);
                    break;
                }
                case Api::IntersectRect:
                case Api::UnionRect: {
                    const auto a = load(arg(1)), b = load(arg(2));
                    Rect r{};
                    if (api == Api::IntersectRect) {
                        r = {std::max(a[0], b[0]), std::max(a[1], b[1]), std::min(a[2], b[2]),
                             std::min(a[3], b[3])};
                    } else if (empty(a)) {
                        r = b;
                    } else if (empty(b)) {
                        r = a;
                    } else {
                        r = {std::min(a[0], b[0]), std::min(a[1], b[1]), std::max(a[2], b[2]),
                             std::max(a[3], b[3])};
                    }
                    if (empty(r)) {
                        r = {};
                        result = 0;
                    }
                    store(arg(0), r);
                    break;
                }
                case Api::EqualRect:
                    result = load(arg(0)) == load(arg(1)) ? 1 : 0;
                    break;
                default:
                    break;
            }
            break;
        }
        case Api::DeleteDC: {
            const auto it = std::find(gdi_.begin(), gdi_.end(), arg(0));
            result = it != gdi_.end() ? 1 : 0;
            if (it != gdi_.end())
                gdi_.erase(it);
            break;
        }
        case Api::VirtualCopy: {
            // (lpvDest, lpvSrc, cbSize, fdwProtect): a driver maps hardware into its address
            // space. PAGE_PHYSICAL (0x400): lpvSrc is a physical address shifted right by 8;
            // area 7 (0x1C000000+) holds the on-chip registers, reached through P4.
            const auto dest = arg(0), source = arg(1), size = arg(2), protect = arg(3);
            if (!dest || !size || (dest & 0xFFFu)) {
                last_error = kInvalidParameter;
                result = 0;
                break;
            }
            std::uint32_t bus = 0;
            if (protect & 0x400u) {
                const std::uint64_t physical = static_cast<std::uint64_t>(source) << 8;
                const auto p = static_cast<std::uint32_t>(physical) & 0x1FFFFFFFu;
                bus = p >= 0x1C000000u ? (0xE0000000u | p) : (0xA0000000u | p);
            } else if (source >= 0x80000000u) {
                bus = source;  // a kernel virtual address: P1/P2/P4 already
            } else {
                last_error = kInvalidParameter;  // aliasing process memory: not supported
                result = 0;
                break;
            }
            const auto span = (size + 0xFFFu) & ~0xFFFu;
            memory_.map_window(dest, span, bus);
            std::fprintf(stderr, "wince: VirtualCopy 0x%08x -> bus 0x%08x (%u bytes)\n", dest, bus,
                         size);
            result = 1;
            break;
        }
        case Api::HalAllocateCommonBufferWithId:
        case Api::HalAllocateCommonBuffer: {
            // (AdapterObject, Length, PPHYSICAL_ADDRESS LogicalAddress, CacheEnabled[, Id]).
            // A DMA buffer: it must be in the console's RAM, where the devices read, so it comes
            // from a pool in the emulated main RAM and the process sees it through a window. The
            // same non-zero Id gives the same buffer back (WDMLIB shares them by Id).
            const bool with_id =
                static_cast<Api>((target - kBase) / 4) == Api::HalAllocateCommonBufferWithId;
            const auto length = arg(1), logical = arg(2), id = with_id ? arg(4) : 0u;
            result = 0;
            if (id)
                if (const auto it = dma_by_id_.find(id); it != dma_by_id_.end()) {
                    put32(logical, it->second.physical);
                    put32(logical + 4, 0);
                    result = it->second.process;
                    break;
                }
            const auto span = (std::max(length, 1u) + 0xFFFu) & ~0xFFFu;
            if (!length || span > kDmaPoolEnd - dma_next_) {
                last_error = 8;  // ERROR_NOT_ENOUGH_MEMORY
                break;
            }
            const auto physical = dma_next_;
            dma_next_ += span;
            const auto process = kDmaWindow + (physical - kDmaPoolStart);
            memory_.map_window(process, span, 0xA0000000u | physical);
            if (id)
                dma_by_id_[id] = DmaBuffer{process, physical};
            put32(logical, physical);
            put32(logical + 4, 0);
            std::fprintf(stderr,
                         "wince: DMA buffer %u bytes at RAM 0x%08x (process 0x%08x, id %u)\n",
                         length, physical, process, id);
            result = process;
            break;
        }
        case Api::HalFreeCommonBufferWithId:
        case Api::HalFreeCommonBuffer:
            break;  // the pool only grows; buffers are few and live as long as the title
        case Api::NKDbgPrintfW: {
            // (format, ...): the kernel's debug output. Printed with a small printf: flags, width,
            // l/h, and d i u x X c s % (s is UTF-16 here, as in every CE wide format).
            const auto format = read_wide(memory, arg(0));
            std::string out;
            unsigned next = 1;
            for (std::size_t i = 0; i < format.size(); ++i) {
                if (format[i] != u'%' || i + 1 == format.size()) {
                    out += format[i] < 128 ? static_cast<char>(format[i]) : '?';
                    continue;
                }
                std::string spec = "%";
                ++i;
                while (i < format.size() &&
                       std::u16string_view(u"-+ #0123456789.").find(format[i]) !=
                           std::u16string_view::npos)
                    spec += static_cast<char>(format[i++]);
                while (i < format.size() && (format[i] == u'l' || format[i] == u'h')) ++i;
                if (i == format.size())
                    break;
                const auto kind = format[i];
                char piece[128] = {};
                if (kind == u'%') {
                    out += '%';
                    continue;
                }
                const auto value = arg(next++);
                if (kind == u's' || kind == u'S') {
                    std::string text;
                    try {
                        text = narrow(read_wide(memory, value, 256));
                    } catch (const std::exception&) {
                        text = "(?)";
                    }
                    out += text;
                    continue;
                }
                if (kind == u'c') {
                    out += value < 128 ? static_cast<char>(value) : '?';
                    continue;
                }
                const char conversion = kind == u'd' || kind == u'i'   ? 'd'
                                        : kind == u'x' || kind == u'p' ? 'x'
                                        : kind == u'X'                 ? 'X'
                                                                       : 'u';
                spec += conversion;
                if (conversion == 'd')
                    std::snprintf(piece, sizeof piece, spec.c_str(), static_cast<int>(value));
                else
                    std::snprintf(piece, sizeof piece, spec.c_str(), value);
                out += piece;
            }
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
            std::fprintf(stderr, "wince: debug: %s\n", out.c_str());
            break;
        }
        // Interrupts: an IST registers an event for a SYSINTR id, waits on it, and calls
        // InterruptDone to re-arm. Which hardware interrupt raises which id is the OEM's (the
        // OAL in nk.exe); interrupt_events() lets the runner signal them.
        case Api::InterruptInitialize: {
            // (idInt, hEvent, pvData, cbData)
            std::fprintf(stderr, "wince: InterruptInitialize(SYSINTR %u, event 0x%08x)\n", arg(0),
                         arg(1));
            interrupts_[arg(0)] = Interrupt{arg(1), true};
            result = 1;
            break;
        }
        case Api::InterruptDone:
            acknowledged_.erase(arg(0));
            if (const auto it = interrupts_.find(arg(0)); it != interrupts_.end())
                it->second.armed = true;
            else if (std::find(unknown_interrupts_.begin(), unknown_interrupts_.end(), arg(0)) ==
                     unknown_interrupts_.end()) {
                unknown_interrupts_.push_back(arg(0));
                std::fprintf(stderr,
                             "wince: InterruptDone(SYSINTR %u) before InterruptInitialize\n",
                             arg(0));
            }
            break;
        case Api::InterruptDisable:
            interrupts_.erase(arg(0));
            break;
        // The display, as GDI and USER report it: the console's 640x480, 16 bits per pixel.
        case Api::GetSystemMetrics:
            result = arg(0) == 0 ? 640u : arg(0) == 1 ? 480u : 0u;  // SM_CXSCREEN, SM_CYSCREEN
            break;
        case Api::GetDeviceCaps:
            switch (arg(1)) {
                case 8:  // HORZRES
                    result = 640;
                    break;
                case 10:  // VERTRES
                    result = 480;
                    break;
                case 12:  // BITSPIXEL
                    result = 16;
                    break;
                case 14:  // PLANES
                    result = 1;
                    break;
                default:
                    result = 0;
                    break;
            }
            break;
        case Api::GetWindowLongW:
        case Api::SetWindowLongW: {
            // (hWnd, nIndex[, dwNewLong]): GWL_WNDPROC (-4) is the window procedure, GWL_STYLE
            // (-16) its style; any other index is a stored value. Set returns the old value.
            const auto it = windows_.find(arg(0));
            if (it == windows_.end()) {
                last_error = 1400;
                result = 0;
                break;
            }
            auto& window = it->second;
            const auto index = static_cast<std::int32_t>(arg(1));
            std::uint32_t& slot = index == -4    ? window.procedure
                                  : index == -16 ? window.style
                                                 : window.longs[index];
            result = slot;
            if (static_cast<Api>((target - kBase) / 4) == Api::SetWindowLongW)
                slot = arg(2);
            break;
        }
        case Api::GetClassInfoW: {
            // (hInstance, lpClassName, lpWndClass): WNDCLASSW as RegisterClassW took it.
            const auto name = arg(1) >= 0x10000 ? read_wide(memory, arg(1)) : std::u16string();
            const auto it = classes_.find(fold(name));
            if (it == classes_.end() || !arg(2)) {
                last_error = 1411;  // ERROR_CLASS_DOES_NOT_EXIST
                result = 0;
                break;
            }
            const std::array<std::uint32_t, 10> wc{it->second.style,
                                                   it->second.procedure,
                                                   0,
                                                   0,
                                                   it->second.instance,
                                                   0,
                                                   0,
                                                   0,
                                                   0,
                                                   arg(1)};
            for (unsigned i = 0; i < wc.size(); ++i) memory.write32(arg(2) + i * 4, wc[i]);
            result = 1;
            break;
        }
        case Api::SetForegroundWindow:
            result = windows_.contains(arg(0)) ? 1 : 0;
            break;
        case Api::SetFocus:
            result = focus_;
            focus_ = arg(0);
            break;
        case Api::CreateBitmapFromPointer: {
            const auto handle = next_gdi_;
            next_gdi_ += 4;
            gdi_.push_back(handle);
            result = handle;
            break;
        }
        case Api::DeleteObject: {
            const auto it = std::find(gdi_.begin(), gdi_.end(), arg(0));
            result = it != gdi_.end() ? 1 : 0;
            if (it != gdi_.end())
                gdi_.erase(it);
            break;
        }
        case Api::MmProbeAndLockPages: {
            // (Mdl, AccessMode, Operation). WDMLIB's MDL (IoAllocateMdl: StartVa +12, ByteCount
            // +16, ByteOffset +20, page frame numbers from +24) describes process memory, which
            // is not on the bus a DMA reads: its bytes are copied into the console-RAM pool now,
            // and the frame numbers (physical >> 12, as SNDCORE turns them back into addresses)
            // name that copy. A device-to-memory transfer would need copying back at unlock;
            // sound playback does not.
            const auto mdl = arg(0);
            const auto start = memory.read32(mdl + 12), count = memory.read32(mdl + 16),
                       offset = memory.read32(mdl + 20);
            const auto pages = (offset + count + 0xFFFu) >> 12;
            const auto span = pages << 12;
            std::uint32_t physical = 0;
            for (auto it = dma_free_.begin(); it != dma_free_.end(); ++it)
                if (it->second >= span) {
                    physical = it->first;
                    dma_free_.erase(it);
                    break;
                }
            if (!physical) {
                if (span > kDmaPoolEnd - dma_next_)
                    throw std::runtime_error("console-RAM pool exhausted by locked pages");
                physical = dma_next_;
                dma_next_ += span;
            }
            const auto bytes = memory_.read_bytes(start + offset, count);
            for (std::uint32_t i = 0; i < count; i += 4) {
                std::uint32_t word = 0;
                for (std::uint32_t b = 0; b < 4 && i + b < count; ++b)
                    word |= static_cast<std::uint32_t>(bytes[i + b]) << (8 * b);
                memory.write32(0xA0000000u | (physical + offset + i), word);
            }
            for (std::uint32_t i = 0; i < pages; ++i)
                memory.write32(mdl + 24 + i * 4, (physical >> 12) + i);
            memory.write16(mdl + 6, static_cast<std::uint16_t>(memory.read16(mdl + 6) | 0x0002u));
            locked_[mdl] = {physical, span};
            break;
        }
        case Api::HalMapInterrupt:
            // (irq, ?, PDWORD pSysIntr, ?) as SNDCORE calls it. In this OAL the numbers are the
            // SYSINTRs themselves (irq 24 is the AICA DMA, SYSINTR 24; see due_interrupts).
            put32(arg(2), arg(0));
            std::fprintf(stderr, "wince: HalMapInterrupt(irq %u) -> SYSINTR %u\n", arg(0), arg(0));
            result = 1;
            break;
        case Api::MmUnlockPages:
            if (const auto it = locked_.find(arg(0)); it != locked_.end()) {
                dma_free_.push_back(it->second);
                locked_.erase(it);
            }
            break;
        case Api::SetStoreQueueBase: {
            // (physical address): what nk.exe (Win32 method 142, 0x8C014958) does. The address
            // must be 1 MiB aligned and below 0xE0000000 (else ERROR_INVALID_PARAMETER and 0).
            // From then on the store-queue area 0xE0000000-0xE3FFFFFF maps onto it onward, a
            // 1 MiB page at a time as its TLB miss handler loads them, and it returns 0xE0000000:
            // drivers add a bus address's distance from `physical` to that.
            const auto physical = arg(0);
            if ((physical & 0x000FFFFFu) || (physical & 0xE0000000u)) {
                last_error = kInvalidParameter;
                result = 0;
                break;
            }
            if (!memory_.set_store_queue_base(physical)) {
                last_error = 50;  // ERROR_NOT_SUPPORTED: no hardware behind the process
                result = 0;
                break;
            }
            std::fprintf(stderr, "wince: store queues 0xE0000000 -> bus 0x%08x onward\n", physical);
            result = 0xE0000000u;
            break;
        }
        case Api::Count:
            return false;
    }
    return true;
}
}  // namespace dream::wince
