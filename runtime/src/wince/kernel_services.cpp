#include "dream/runtime/wince/kernel_services.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <limits>
#include <string_view>

#include "dream/runtime/wince/abi.h"

namespace dream::wince {
namespace {
constexpr std::uint32_t kBase = 0x1F002000, kStringLimit = 1u << 20;
#define WINCE_SERVICES(X)                                                                          \
    X(LocalAlloc)                                                                                  \
    X(LocalFree)                                                                                   \
    X(LocalReAlloc)                                                                                \
    X(LocalSize) X(GetProcessHeap) X(HeapCreate) X(HeapDestroy) X(HeapAlloc) X(HeapFree)           \
        X(HeapReAlloc) X(HeapSize) X(HeapValidate) X(malloc) X(calloc) X(realloc) X(free)          \
            X(VirtualAlloc) X(VirtualFree) X(VirtualProtect) X(VirtualQuery) X(memcpy) X(memmove)  \
                X(memset) X(memcmp) X(strlen) X(strcpy) X(strcat) X(strncpy) X(strcmp) X(strncmp)  \
                    X(strchr) X(strrchr) X(wcslen) X(wcscpy) X(wcscat) X(wcsncpy) X(wcscmp)        \
                        X(wcsncmp) X(wcschr) X(wcsrchr) X(_wcsicmp) X(_wcsnicmp) X(lstrcmpW)       \
                            X(lstrcmpiW) X(CharPrevW) X(InterlockedIncrement)                      \
                                X(InterlockedDecrement) X(InterlockedExchange)                     \
                                    X(InterlockedCompareExchange) X(InterlockedTestExchange)       \
                                        X(QueryPerformanceCounter) X(QueryPerformanceFrequency)    \
                                            X(OutputDebugStringW) X(wsprintfW) X(wvsprintfW)       \
                                                X(_divls) X(_divlu) X(_modls) X(_modlu) X(_divi64) \
                                                    X(_divui64) X(_modi64) X(_modui64) X(_addui64) \
                                                        X(_subui64) X(_muli64) X(_mului64)         \
                                                            X(_geui64) X(_gti64) X(_gtui64)        \
                                                                X(_lti64) X(_i64toi) X(_ui64toi)   \
                                                                    X(_i64toui) X(_ui64toui)       \
                                                                        X(_uitoui64) X(_uitoi64)   \
                                                                            X(_itoui64) X(_itoi64) \
                                                                                X(_itos) X(_utos)  \
                                                                                    X(_stoi)       \
                                                                                        X(_stou)   \
    X(_lshi64) X(_rshi64) X(_rshui64) X(rand) X(srand) X(strstr) X(toupper) X(atol) X(iswctype)    \
        X(_wcsupr) X(mbstowcs) X(wcstombs) X(MultiByteToWideChar) X(swprintf) X(GetLocalTime)      \
            X(GetSystemTime)
#define ENUM(name) name,
enum class Api { WINCE_SERVICES(ENUM) Count };
#undef ENUM
#define NAME(name) #name,
constexpr std::array<std::string_view, static_cast<unsigned>(Api::Count)> kNames{
    WINCE_SERVICES(NAME)};
#undef NAME
#undef WINCE_SERVICES

void require(bool ok, std::uint32_t error = 87) {
    if (!ok)
        throw MemoryError(error, "unsupported or invalid COREDLL arguments");
}
std::uint32_t at(std::uint32_t base, std::uint32_t offset) {
    if (!base || static_cast<std::uint64_t>(base) + offset >= 0x80000000ull)
        throw std::runtime_error("invalid CRT guest pointer");
    return base + offset;
}
std::uint32_t unit(Memory& m, std::uint32_t base, std::uint32_t index, bool wide) {
    return wide ? m.read16(at(base, index * 2)) : m.read8(at(base, index));
}
std::vector<std::uint32_t> string(Memory& m, std::uint32_t base, bool wide,
                                  std::uint32_t count = kStringLimit, bool bounded = false) {
    require(count <= kStringLimit);
    std::vector<std::uint32_t> text;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto c = unit(m, base, i, wide);
        if (!c)
            return text;
        text.push_back(c);
    }
    if (!bounded)
        throw std::runtime_error("unterminated CRT string");
    return text;
}
void write_string(ProcessMemory& m, std::uint32_t base, const std::vector<std::uint32_t>& text,
                  bool wide) {
    const auto stride = wide ? 2u : 1u;
    std::vector<std::uint8_t> bytes(text.size() * stride);
    for (std::size_t i = 0; i < text.size(); ++i) {
        bytes[i * stride] = static_cast<std::uint8_t>(text[i]);
        if (wide)
            bytes[i * stride + 1] = static_cast<std::uint8_t>(text[i] >> 8);
    }
    if (!bytes.empty())
        at(base, static_cast<std::uint32_t>(bytes.size() - 1));
    if (wide && (base & 1))
        throw std::runtime_error("unaligned UTF-16 destination");
    m.write_bytes(base, bytes);
}
std::uint32_t fold(std::uint32_t c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}
// The character classes iswctype tests, in the CRT's bits (_UPPER 1, _LOWER 2, _DIGIT 4,
// _SPACE 8, _PUNCT 0x10, _CONTROL 0x20, _BLANK 0x40, _HEX 0x80, alphabetic 0x100): the C locale,
// and Latin-1 above it as the wide functions see it (accented letters are letters).
std::uint32_t ctype(std::uint32_t c) {
    if (c < 0x80) {
        std::uint32_t t = 0;
        if (c >= 'A' && c <= 'Z')
            t |= 0x101;
        if (c >= 'a' && c <= 'z')
            t |= 0x102;
        if (c >= '0' && c <= '9')
            t |= 0x84;
        if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
            t |= 0x80;
        if (c == ' ' || (c >= 9 && c <= 13))
            t |= 8;
        if (c == ' ' || c == 9)
            t |= 0x40;
        if (c < 0x20 || c == 0x7F)
            t |= 0x20;
        if (c > 0x20 && c < 0x7F && !(t & 0x107))
            t |= 0x10;
        return t;
    }
    if (c < 0xA0)
        return 0x20;  // C1 controls
    if (c == 0xA0)
        return 0x48;  // no-break space
    if (c > 0xFF)
        return 0;
    if (c < 0xC0 || c == 0xD7 || c == 0xF7)
        return 0x10;
    return c < 0xDF ? 0x101u : 0x102u;
}
std::uint32_t upper(std::uint32_t c) {
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7))
        return c - 32;
    return c == 0xFF ? 0x178u : c;
}
std::uint32_t compare(Memory& m, std::uint32_t a, std::uint32_t b, bool wide, bool insensitive,
                      std::uint32_t count = kStringLimit, bool bounded = false) {
    require(count <= kStringLimit);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto x = unit(m, a, i, wide), y = unit(m, b, i, wide);
        if (insensitive) {
            x = fold(x);
            y = fold(y);
        }
        if (x != y)
            return static_cast<std::uint32_t>(static_cast<int>(x) - static_cast<int>(y));
        if (!x)
            return 0;
    }
    if (!bounded)
        throw std::runtime_error("unterminated CRT comparison");
    return 0;
}
std::vector<std::uint32_t> formatted(Memory& m, std::uint32_t fmt,
                                     const std::function<std::uint32_t()>& next) {
    const auto format = string(m, fmt, true, 512);
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%')
            out.push_back(format[i]);
        else {
            require(++i < format.size());
            if (format[i] == '%') {
                out.push_back('%');
                require(out.size() <= 511, 122);
                continue;
            }
            bool left = false, zero = false;
            while (i < format.size() && (format[i] == '-' || format[i] == '0')) {
                if (format[i] == '-')
                    left = true;
                else
                    zero = true;
                ++i;
            }
            const auto number = [&]() {
                std::uint32_t n = 0;
                while (i < format.size() && format[i] >= '0' && format[i] <= '9') {
                    n = n * 10 + format[i++] - '0';
                    require(n <= 511, 122);
                }
                return n;
            };
            const auto width = number();
            std::optional<std::uint32_t> precision;
            if (i < format.size() && format[i] == '.') {
                ++i;
                precision = number();
            }
            std::uint32_t length = 0;
            if (i < format.size() && (format[i] == 'l' || format[i] == 'h' || format[i] == 'w'))
                length = format[i++];
            require(i < format.size());
            const auto type = format[i];
            std::vector<std::uint32_t> text;
            bool negative = false, numeric = false;
            if (type == 's' || type == 'S' || type == 'c' || type == 'C') {
                const bool wide = length == 'l' || length == 'w' ||
                                  (length != 'h' && (type == 's' || type == 'c'));
                const auto value = next();
                if (type == 'c' || type == 'C') {
                    if (value & (wide ? 65535u : 255u))
                        text.push_back(value & (wide ? 65535u : 255u));
                } else if (!precision || *precision) {
                    require(value != 0, 998);
                    text = string(m, value, wide, precision.value_or(512), precision.has_value());
                }
                if (!wide)
                    for (auto ch : text) require(ch < 128, 120);  // No guessed ANSI code page.
            } else if (type == 'd' || type == 'i' || type == 'u' || type == 'x' || type == 'X') {
                require(length == 0 || length == 'l', 120);
                numeric = true;
                auto value = next();
                if ((type == 'd' || type == 'i') && (value & 0x80000000u)) {
                    negative = true;
                    value = 0u - value;
                }
                const unsigned radix = type == 'x' || type == 'X' ? 16 : 10;
                const char* digits = type == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
                if (value || !precision || *precision)
                    do {
                        text.push_back(static_cast<unsigned>(digits[value % radix]));
                        value /= radix;
                    } while (value);
                while (precision && text.size() < *precision) text.push_back('0');
                std::reverse(text.begin(), text.end());
            } else
                throw MemoryError(120, "unsupported wsprintf format conversion");
            const auto required = text.size() + (negative ? 1u : 0u);
            const auto padding = width > required ? width - required : 0u;
            require(out.size() + required + padding <= 511, 122);
            const bool zero_pad = numeric && zero && !left && !precision;
            if (!left && !zero_pad)
                out.insert(out.end(), padding, ' ');
            if (negative)
                out.push_back('-');
            if (zero_pad)
                out.insert(out.end(), padding, '0');
            out.insert(out.end(), text.begin(), text.end());
            if (left)
                out.insert(out.end(), padding, ' ');
        }
        require(out.size() <= 511, 122);
    }
    return out;
}
}  // namespace

std::optional<std::uint32_t> KernelServices::resolve(const std::string& name) {
    const auto it = std::find(kNames.begin(), kNames.end(), name);
    if (it == kNames.end())
        return {};
    return kBase + static_cast<std::uint32_t>(it - kNames.begin()) * 4;
}
bool KernelServices::handles(std::uint32_t target) {
    return target >= kBase && target < kBase + kNames.size() * 4 && !(target & 3);
}
bool KernelServices::invoke(sh4::Ctx& c, Memory& memory, std::uint32_t target,
                            std::uint32_t& error) {
    if (!handles(target))
        return false;
    if (&memory != &memory_)
        throw std::runtime_error("COREDLL services used with another process");
    const auto api = static_cast<Api>((target - kBase) / 4);
    const auto a = c.r[4], b = c.r[5], d = c.r[6], e = c.r[7];
    auto& result = c.r[0];
    const auto local = Heaps::Family::Local, crt = Heaps::Family::Crt;
    // These APIs have Win32 failure values. CRT faults below propagate instead of pretending
    // that a copy, free or interlocked access to invalid memory completed successfully.
    if (api <= Api::HeapValidate || (api >= Api::VirtualAlloc && api <= Api::VirtualQuery) ||
        api == Api::QueryPerformanceCounter || api == Api::QueryPerformanceFrequency ||
        api == Api::wsprintfW || api == Api::wvsprintfW || api == Api::MultiByteToWideChar) {
        const auto failure = api == Api::LocalFree ? a : api == Api::HeapSize ? 0xFFFFFFFFu : 0u;
        result = failure;
        try {
            switch (api) {
                case Api::LocalAlloc:
                    // The flags COREDLL's own refuses (ERROR_INVALID_PARAMETER): the others,
                    // LMEM_MOVEABLE among them, give fixed memory, as on CE. CEMM.DLL asks for
                    // movable memory when it loads the MIDI driver; refused, Sega Rally 2 had no
                    // MIDI and no sound effects.
                    require(!(a & 0xFFFFF08Du));
                    result = heaps_.allocate(Heaps::kProcess, b, true, local);
                    break;
                case Api::LocalFree:
                    if (a) {
                        try {
                            heaps_.free(Heaps::kProcess, a, local);
                        } catch (const MemoryError&) {
                            throw MemoryError(6, "invalid LocalFree handle");
                        }
                    }
                    result = 0;
                    break;
                case Api::LocalReAlloc:
                    // As COREDLL's: these bits refused, and discardable ones only with LMEM_MODIFY.
                    require(!(d & 0xFFFFF00Du) && (!(d & 0xF00u) || (d & 0x80u)));
                    result =
                        heaps_.reallocate(Heaps::kProcess, a, b, (d & 0x40) != 0, !(d & 2), local);
                    break;
                case Api::LocalSize:
                    result = heaps_.size(Heaps::kProcess, a, local);
                    break;
                case Api::GetProcessHeap:
                    result = Heaps::kProcess;
                    break;
                case Api::HeapCreate:
                    require(!(a & ~1u), 120);
                    result = heaps_.create(b, d);
                    break;
                case Api::HeapDestroy:
                    heaps_.destroy(a);
                    result = 1;
                    break;
                case Api::HeapAlloc:
                    require(!(b & ~9u), 120);
                    result = heaps_.allocate(a, d, (b & 8) != 0);
                    break;
                case Api::HeapFree:
                    require(!(b & ~1u), 120);
                    heaps_.free(a, d);
                    result = 1;
                    break;
                case Api::HeapReAlloc:
                    require(!(b & ~0x19u), 120);
                    result = heaps_.reallocate(a, d, e, (b & 8) != 0, (b & 0x10) != 0);
                    break;
                case Api::HeapSize:
                    require(!(b & ~1u), 120);
                    result = heaps_.size(a, d);
                    break;
                case Api::HeapValidate:
                    require(!(b & ~1u), 120);
                    result = heaps_.validate(a, d);
                    if (!result)
                        error = 87;
                    break;
                case Api::VirtualAlloc:
                    result = memory_.virtual_alloc(a, b, d, e);
                    break;
                case Api::VirtualFree:
                    memory_.virtual_free(a, b, d);
                    result = 1;
                    break;
                case Api::VirtualProtect:
                    memory_.virtual_protect(a, b, d, e);
                    result = 1;
                    break;
                case Api::VirtualQuery: {
                    require(d >= 28 && b && !(b & 3));
                    const auto info = memory_.virtual_query(a);
                    memory_.check(b, 28, ProcessMemory::Write);
                    for (unsigned i = 0; i < 7; ++i) memory_.write32(b + i * 4, info[i]);
                    result = 28;
                    break;
                }
                case Api::QueryPerformanceCounter:
                case Api::QueryPerformanceFrequency:
                    require(a && !(a & 3), 998);
                    memory_.write64(a,
                                    api == Api::QueryPerformanceCounter ? c.cycles : 200000000ull);
                    result = 1;
                    break;
                case Api::MultiByteToWideChar: {
                    // (code page, flags, source, its bytes or -1 to its terminator, destination,
                    // its characters or 0 for the size needed). The ANSI and OEM pages are taken
                    // as Latin-1, as COREDLL's mbstowcs does; UTF-8 is decoded.
                    const auto out = argument(c, memory_, 4), room = argument(c, memory_, 5);
                    require(d && e != 0 &&
                            (a == 0 || a == 1 || a == 1252 || a == 28591 || a == 65001));
                    std::vector<std::uint32_t> bytes;
                    if (e == 0xFFFFFFFFu) {
                        bytes = string(memory_, d, false);
                        bytes.push_back(0);
                    } else {
                        require(e <= kStringLimit);
                        for (std::uint32_t i = 0; i < e; ++i)
                            bytes.push_back(unit(memory_, d, i, false));
                    }
                    std::vector<std::uint32_t> wide;
                    for (std::size_t i = 0; i < bytes.size(); ++i) {
                        auto ch = bytes[i];
                        if (a == 65001 && ch >= 0x80) {
                            // A lead byte and its continuations; a malformed one is U+FFFD.
                            const unsigned more = ch >= 0xF0   ? 3
                                                  : ch >= 0xE0 ? 2
                                                  : ch >= 0xC0 ? 1
                                                               : 0;
                            std::uint32_t cp = ch & (0x3Fu >> more);
                            bool ok = more != 0 && i + more < bytes.size();
                            for (unsigned k = 1; ok && k <= more; ++k) {
                                ok = (bytes[i + k] & 0xC0) == 0x80;
                                cp = cp << 6 | (bytes[i + k] & 0x3F);
                            }
                            if (!ok) {
                                wide.push_back(0xFFFD);
                                continue;
                            }
                            i += more;
                            if (cp >= 0x10000) {
                                cp -= 0x10000;
                                wide.push_back(0xD800 + (cp >> 10));
                                ch = 0xDC00 + (cp & 0x3FF);
                            } else
                                ch = cp;
                        }
                        wide.push_back(ch);
                    }
                    const auto count = static_cast<std::uint32_t>(wide.size());
                    if (!room) {
                        result = count;
                        break;
                    }
                    require(out && !(out & 1));
                    if (count > room) {
                        error = 122;  // ERROR_INSUFFICIENT_BUFFER
                        break;
                    }
                    write_string(memory_, out, wide, true);
                    result = count;
                    break;
                }
                case Api::wsprintfW:
                case Api::wvsprintfW: {
                    unsigned index = 0;
                    auto output = formatted(memory_, b, [&]() {
                        if (api == Api::wsprintfW)
                            return argument(c, memory_, 2 + index++);
                        return memory_.read32(at(d, index++ * 4));
                    });
                    const auto count = static_cast<std::uint32_t>(output.size());
                    output.push_back(0);
                    write_string(memory_, a, output, true);
                    result = count;
                    break;
                }
                default:
                    break;
            }
        } catch (const MemoryError& ex) {
            result = failure;
            error = ex.code;
        } catch (const std::bad_alloc&) {
            result = failure;
            error = 8;
        } catch (const std::runtime_error&) {
            result = failure;
            error = 998;
        }
        return true;
    }
    switch (api) {
        case Api::_itos:
        case Api::_utos:
        case Api::_stoi:
        case Api::_stou: {
            // The compiler's float conversions (Sega Rally 2's race code): an integer in R4 to
            // a float in FR0, or a float in FR4 to an integer in R0, truncated toward zero as C
            // converts. Out of range, the nearest bound, as the SH-4's FTRC saturates; NaN gives
            // what FTRC gives for it (0x80000000), and 0 for an unsigned result.
            if (api == Api::_itos) {
                c.fr[0] = static_cast<float>(static_cast<std::int32_t>(a));
            } else if (api == Api::_utos) {
                c.fr[0] = static_cast<float>(a);
            } else {
                const double v = c.fr[4];
                if (api == Api::_stoi)
                    result = v != v              ? 0x80000000u
                             : v >= 2147483648.0 ? 0x7FFFFFFFu
                             : v <= -2147483649.0
                                 ? 0x80000000u
                                 : static_cast<std::uint32_t>(static_cast<std::int32_t>(v));
                else
                    result = v != v || v <= -1.0 ? 0u
                             : v >= 4294967296.0 ? 0xFFFFFFFFu
                                                 : static_cast<std::uint32_t>(v);
            }
            break;
        }
        case Api::_divls:
        case Api::_divlu:
        case Api::_modls:
        case Api::_modlu: {
            // Microsoft SH-4 helper ABI: R4 numerator, R5 divisor, R0 result.
            // Widen before signed division so INT_MIN / -1 cannot overflow host C++.
            if (!b)
                throw std::runtime_error("SH-4 division by zero requires WinCE exception dispatch");
            if (api == Api::_divlu || api == Api::_modlu)
                result = api == Api::_divlu ? a / b : a % b;
            else {
                const auto signed32 = [](std::uint32_t value) -> std::int64_t {
                    return value & 0x80000000u ? static_cast<std::int64_t>(value) - 0x100000000ll
                                               : value;
                };
                const auto numerator = signed32(a), divisor = signed32(b);
                result = static_cast<std::uint32_t>(api == Api::_divls ? numerator / divisor
                                                                       : numerator % divisor);
            }
            break;
        }
        case Api::_divi64:
        case Api::_divui64:
        case Api::_modi64:
        case Api::_modui64:
        case Api::_addui64:
        case Api::_subui64:
        case Api::_muli64:
        case Api::_mului64:
        case Api::_geui64:
        case Api::_gti64:
        case Api::_gtui64:
        case Api::_lti64:
        case Api::_i64toi:
        case Api::_ui64toi:
        case Api::_i64toui:
        case Api::_ui64toui:
        case Api::_uitoui64:
        case Api::_uitoi64:
        case Api::_itoui64:
        case Api::_itoi64: {
            // Microsoft SH-4 __int64 helpers: the values live in memory, low word first.
            // Arithmetic takes (result*, a*, b*), comparisons (a*, b*) and give R0, conversions
            // to 32 bits (a*) and give R0, conversions from 32 bits (result*, value). Read from
            // their call sites in MGameStorage (_muli64, _uitoi64) and MGameSound (_divui64,
            // _geui64, _ui64toui, _uitoui64).
            const auto load = [&](std::uint32_t p) {
                return static_cast<std::uint64_t>(memory_.read32(at(p, 0))) |
                       static_cast<std::uint64_t>(memory_.read32(at(p, 4))) << 32;
            };
            const auto store = [&](std::uint32_t p, std::uint64_t v) {
                memory_.write32(at(p, 0), static_cast<std::uint32_t>(v));
                memory_.write32(at(p, 4), static_cast<std::uint32_t>(v >> 32));
            };
            const auto sign = [](std::uint64_t v) { return static_cast<std::int64_t>(v); };
            switch (api) {
                case Api::_geui64:
                    result = load(a) >= load(b);
                    break;
                case Api::_gti64:
                    result = sign(load(a)) > sign(load(b));
                    break;
                case Api::_gtui64:
                    result = load(a) > load(b);
                    break;
                case Api::_lti64:
                    result = sign(load(a)) < sign(load(b));
                    break;
                case Api::_i64toi:
                case Api::_ui64toi:
                case Api::_i64toui:
                case Api::_ui64toui:
                    result = static_cast<std::uint32_t>(load(a));  // the low word, as truncated
                    break;
                case Api::_uitoui64:
                case Api::_uitoi64:
                    store(a, b);
                    result = a;
                    break;
                case Api::_itoui64:
                case Api::_itoi64:
                    store(a, static_cast<std::uint64_t>(
                                 static_cast<std::int64_t>(static_cast<std::int32_t>(b))));
                    result = a;
                    break;
                default: {
                    const auto x = load(b), y = load(d);
                    std::uint64_t r = 0;
                    if ((api == Api::_divi64 || api == Api::_divui64 || api == Api::_modi64 ||
                         api == Api::_modui64) &&
                        !y)
                        throw std::runtime_error(
                            "SH-4 division by zero requires WinCE exception dispatch");
                    const bool overflow =
                        sign(x) == std::numeric_limits<std::int64_t>::min() && sign(y) == -1;
                    switch (api) {
                        case Api::_divi64:
                            r = overflow ? x : static_cast<std::uint64_t>(sign(x) / sign(y));
                            break;
                        case Api::_modi64:
                            r = overflow ? 0 : static_cast<std::uint64_t>(sign(x) % sign(y));
                            break;
                        case Api::_divui64:
                            r = x / y;
                            break;
                        case Api::_modui64:
                            r = x % y;
                            break;
                        case Api::_addui64:
                            r = x + y;
                            break;
                        case Api::_subui64:
                            r = x - y;
                            break;
                        default:  // _muli64, _mului64: the low 64 bits are the same either way
                            r = x * y;
                            break;
                    }
                    store(a, r);
                    result = a;
                    break;
                }
            }
            break;
        }
        case Api::malloc:
        case Api::calloc:
        case Api::realloc: {
            try {
                if (api == Api::realloc && a && !b) {
                    heaps_.free(Heaps::kProcess, a, crt);
                    result = 0;
                } else if (api == Api::realloc && a)
                    result = heaps_.reallocate(Heaps::kProcess, a, b, false, false, crt);
                else {
                    const auto size = api == Api::calloc   ? static_cast<std::uint64_t>(a) * b
                                      : api == Api::malloc ? a
                                                           : b;
                    require(size <= ProcessMemory::kBudget, 8);
                    result = heaps_.allocate(Heaps::kProcess, static_cast<std::uint32_t>(size),
                                             api == Api::calloc, crt);
                }
            } catch (const MemoryError& ex) {
                if (ex.code != 8)
                    throw;
                result = 0;
            } catch (const std::bad_alloc&) {
                result = 0;
            }
            break;
        }
        case Api::free:
            if (a)
                heaps_.free(Heaps::kProcess, a, crt);
            result = 0;
            break;
        case Api::memcpy:
        case Api::memmove:
            require(d <= ProcessMemory::kBudget);
            if (d) {
                at(a, d - 1);
                at(b, d - 1);
            }
            memory_.write_bytes(a, memory_.read_bytes(b, d));
            result = a;
            break;
        case Api::memset:
            require(d <= ProcessMemory::kBudget);
            if (d)
                at(a, d - 1);
            memory_.write_bytes(a, std::vector<std::uint8_t>(d, static_cast<std::uint8_t>(b)));
            result = a;
            break;
        case Api::memcmp: {
            require(d <= ProcessMemory::kBudget);
            if (d) {
                at(a, d - 1);
                at(b, d - 1);
            }
            const auto x = memory_.read_bytes(a, d), y = memory_.read_bytes(b, d);
            result = 0;
            for (std::size_t i = 0; i < x.size(); ++i)
                if (x[i] != y[i]) {
                    result = static_cast<std::uint32_t>(static_cast<int>(x[i]) - y[i]);
                    break;
                }
            break;
        }
        case Api::strlen:
        case Api::wcslen:
            result = static_cast<std::uint32_t>(string(memory_, a, api == Api::wcslen).size());
            break;
        case Api::strcpy:
        case Api::wcscpy:
        case Api::strcat:
        case Api::wcscat:
        case Api::strncpy:
        case Api::wcsncpy: {
            const bool wide = api == Api::wcscpy || api == Api::wcscat || api == Api::wcsncpy;
            const bool bounded = api == Api::strncpy || api == Api::wcsncpy;
            auto text = string(memory_, b, wide, bounded ? d : kStringLimit, bounded);
            if (bounded)
                text.resize(d, 0);
            else
                text.push_back(0);
            auto dest = a;
            if (api == Api::strcat || api == Api::wcscat)
                dest = at(a, static_cast<std::uint32_t>(string(memory_, a, wide).size()) *
                                 (wide ? 2u : 1u));
            write_string(memory_, dest, text, wide);
            result = a;
            break;
        }
        case Api::strcmp:
        case Api::wcscmp:
        case Api::strncmp:
        case Api::wcsncmp:
        case Api::_wcsicmp:
        case Api::_wcsnicmp:
        case Api::lstrcmpW:
        case Api::lstrcmpiW: {
            const bool wide = api != Api::strcmp && api != Api::strncmp;
            const bool bounded =
                api == Api::strncmp || api == Api::wcsncmp || api == Api::_wcsnicmp;
            const bool insensitive =
                api == Api::_wcsicmp || api == Api::_wcsnicmp || api == Api::lstrcmpiW;
            if ((api == Api::lstrcmpW || api == Api::lstrcmpiW) && (!a || !b)) {
                // The Win32 comparisons (not the CRT's) take a null string as less than any
                // other, as Windows does: Sega Rally 2 compares a file's name with a name its
                // own table leaves empty for one of its cars, as it loads a stage.
                result = !a && !b ? 0u : !a ? 0xFFFFFFFFu : 1u;
                break;
            }
            if (api == Api::lstrcmpW || api == Api::lstrcmpiW) {
                // NLS sorting is not implemented. Avoid silently claiming correct linguistic
                // comparisons for non-ASCII text through the Windows (rather than CRT) API.
                for (auto p : {a, b})
                    for (auto ch : string(memory_, p, true)) require(ch < 128, 120);
            }
            result = compare(memory_, a, b, wide, insensitive, bounded ? d : kStringLimit, bounded);
            break;
        }
        case Api::strchr:
        case Api::strrchr:
        case Api::wcschr:
        case Api::wcsrchr: {
            const bool wide = api == Api::wcschr || api == Api::wcsrchr;
            const bool last = api == Api::strrchr || api == Api::wcsrchr;
            const auto sought = b & (wide ? 65535u : 255u);
            result = 0;
            for (std::uint32_t i = 0; i < kStringLimit; ++i) {
                const auto value = unit(memory_, a, i, wide);
                if (value == sought) {
                    result = at(a, i * (wide ? 2u : 1u));
                    if (!last)
                        break;
                }
                if (!value)
                    return true;
                if (i + 1 == kStringLimit)
                    throw std::runtime_error("unterminated CRT search");
            }
            break;
        }
        case Api::_lshi64:
        case Api::_rshi64:
        case Api::_rshui64: {
            // (result*, value*, count), as COREDLL's own, which shifts one bit `count` times:
            // 64 and more leave 0, or the sign.
            std::uint64_t v = static_cast<std::uint64_t>(memory_.read32(at(b, 0))) |
                              static_cast<std::uint64_t>(memory_.read32(at(b, 4))) << 32;
            if (api == Api::_lshi64)
                v = d >= 64 ? 0 : v << d;
            else if (api == Api::_rshui64)
                v = d >= 64 ? 0 : v >> d;
            else
                v = static_cast<std::uint64_t>(static_cast<std::int64_t>(v) >> (d >= 64 ? 63 : d));
            memory_.write32(at(a, 0), static_cast<std::uint32_t>(v));
            memory_.write32(at(a, 4), static_cast<std::uint32_t>(v >> 32));
            result = a;
            break;
        }
        case Api::srand:
            rand_seed_ = a;
            result = 0;
            break;
        case Api::rand:
            // The CRT's generator, as COREDLL's (Worms Armageddon).
            rand_seed_ = rand_seed_ * 214013u + 2531011u;
            result = (rand_seed_ >> 16) & 0x7FFFu;
            break;
        case Api::strstr: {
            const auto text = string(memory_, a, false), sought = string(memory_, b, false);
            const auto it = std::search(text.begin(), text.end(), sought.begin(), sought.end());
            result = it == text.end() && !sought.empty()
                         ? 0u
                         : a + static_cast<std::uint32_t>(it - text.begin());
            break;
        }
        case Api::toupper:
            result = a >= 'a' && a <= 'z' ? a - 32 : a;  // the C locale
            break;
        case Api::atol: {
            // As COREDLL's: white space, a sign, digits; no overflow check.
            std::uint32_t i = 0, value = 0;
            auto ch = unit(memory_, a, i, false);
            while (ch == ' ' || (ch >= 9 && ch <= 13)) ch = unit(memory_, a, ++i, false);
            const bool minus = ch == '-';
            if (ch == '-' || ch == '+')
                ch = unit(memory_, a, ++i, false);
            for (; ch >= '0' && ch <= '9'; ch = unit(memory_, a, ++i, false))
                value = value * 10 + (ch - '0');
            result = minus ? 0u - value : value;
            break;
        }
        case Api::iswctype:
            result = (a & 0xFFFF) == 0xFFFF ? 0u : ctype(a & 0xFFFF) & b;  // WEOF: none
            break;
        case Api::_wcsupr: {
            auto text = string(memory_, a, true);
            for (auto& ch : text) ch = upper(ch);
            write_string(memory_, a, text, true);
            result = a;
            break;
        }
        case Api::mbstowcs:
        case Api::wcstombs: {
            // COREDLL's own: a byte is the character of the same code (Latin-1), a character
            // above 0xFF fails (-1). Without a destination they give the size the whole string
            // takes with its terminator, in bytes: (length + 1) * 2 and length + 1. With one,
            // at most `d` characters, terminated when there is room, and the count without it.
            const bool to_wide = api == Api::mbstowcs;
            const auto text = a ? string(memory_, b, !to_wide, std::min(d, kStringLimit), true)
                                : string(memory_, b, !to_wide);
            const auto wide_only = [](std::uint32_t ch) { return ch > 0xFF; };
            if (!a) {
                const auto n = static_cast<std::uint32_t>(text.size()) + 1;
                result = !to_wide && std::any_of(text.begin(), text.end(), wide_only) ? 0xFFFFFFFFu
                         : to_wide                                                    ? n * 2
                                                                                      : n;
                break;
            }
            std::vector<std::uint32_t> copy;
            bool failed = false;
            for (const auto ch : text) {
                if (!to_wide && wide_only(ch)) {
                    failed = true;  // what came before it is written, as the guest's loop does
                    break;
                }
                copy.push_back(ch);
            }
            result = failed ? 0xFFFFFFFFu : static_cast<std::uint32_t>(copy.size());
            if (!failed && copy.size() < d)
                copy.push_back(0);
            if (!copy.empty())
                write_string(memory_, a, copy, to_wide);
            break;
        }
        case Api::swprintf: {
            // swprintf(buffer, format, ...): wsprintfW's conversions; anything else faults.
            unsigned index = 0;
            auto output =
                formatted(memory_, b, [&]() { return argument(c, memory_, 2 + index++); });
            const auto count = static_cast<std::uint32_t>(output.size());
            output.push_back(0);
            write_string(memory_, a, output, true);
            result = count;
            break;
        }
        case Api::GetLocalTime:
        case Api::GetSystemTime: {
            // SYSTEMTIME from the host's clock, as the console's own clock would give it.
            const auto now = std::chrono::system_clock::now();
            const std::time_t t = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
#ifdef _WIN32
            if (api == Api::GetLocalTime)
                localtime_s(&tm, &t);
            else
                gmtime_s(&tm, &t);
#else
            if (api == Api::GetLocalTime)
                localtime_r(&t, &tm);
            else
                gmtime_r(&t, &tm);
#endif
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
                    .count() %
                1000;
            const std::array<std::uint32_t, 8> fields{static_cast<std::uint32_t>(tm.tm_year + 1900),
                                                      static_cast<std::uint32_t>(tm.tm_mon + 1),
                                                      static_cast<std::uint32_t>(tm.tm_wday),
                                                      static_cast<std::uint32_t>(tm.tm_mday),
                                                      static_cast<std::uint32_t>(tm.tm_hour),
                                                      static_cast<std::uint32_t>(tm.tm_min),
                                                      static_cast<std::uint32_t>(tm.tm_sec),
                                                      static_cast<std::uint32_t>(ms)};
            for (unsigned i = 0; i < fields.size(); ++i)
                memory_.write16(at(a, i * 2), static_cast<std::uint16_t>(fields[i]));
            result = 0;
            break;
        }
        case Api::CharPrevW:
            require(a && b && !(a & 1) && !(b & 1) && b >= a);
            result = b == a ? b : b - 2;
            break;
        case Api::InterlockedIncrement:
        case Api::InterlockedDecrement:
        case Api::InterlockedExchange:
        case Api::InterlockedCompareExchange:
        case Api::InterlockedTestExchange: {
            // The guest has one thread. Every operation is indivisible relative to interpreter
            // execution; a future scheduler must keep this whole dispatch atomic.
            const auto old = memory_.read32(a);
            memory_.check(a, 4, ProcessMemory::Write);
            auto value = old;
            result = old;
            if (api == Api::InterlockedIncrement)
                result = value = old + 1;
            else if (api == Api::InterlockedDecrement)
                result = value = old - 1;
            else if (api == Api::InterlockedExchange)
                value = b;
            else if (api == Api::InterlockedCompareExchange && old == d)
                value = b;
            else if (api == Api::InterlockedTestExchange && old == b)
                value = d;
            memory_.write32(a, value);
            break;
        }
        case Api::OutputDebugStringW: {
            const auto text = string(memory_, a, true, 4096);
            if (debug_.size() >= 256)
                debug_.erase(debug_.begin());
            std::u16string message;
            for (auto ch : text) message += static_cast<char16_t>(ch);
            debug_.push_back(std::move(message));
            result = 0;
            break;
        }
        default:
            throw std::logic_error("unhandled COREDLL service");
    }
    return true;
}
}  // namespace dream::wince
