#include "dream/runtime/wince/directx.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <stdexcept>

#include "dream/runtime/wince/abi.h"

namespace dream::wince {
namespace {
constexpr std::uint32_t kFactory = 0x1F100000, kMethods = 0x1F101000;
constexpr std::uint32_t kInvalid = 0x80070057, kPointer = 0x80004003, kNoInterface = 0x80004002;
constexpr std::uint32_t kUnsupported = 0x80004001, kOutOfMemory = 0x8007000E;
constexpr std::uint32_t kBusy = 0x887601AE, kNoAggregation = 0x80040110;
enum class Kind : unsigned { Draw1, Draw4, Surface1, Surface4, D3D3, Device3, Sound, Buffer };
enum class Family { Draw, Surface, Device, Sound, Buffer };
using Guid = std::array<std::uint32_t, 4>;
// Little-endian in-memory GUID words; derived from the public DirectX interface definitions.
constexpr Guid kUnknown{0, 0, 0xC0, 0x46000000};
constexpr std::array<Guid, 8> kIids{{{0x6C14DB80, 0x11CEA733, 0x200021A5, 0x60E50BAF},
                                     {0x9C59509A, 0x11D139BD, 0xC0004A8C, 0xC530D94F},
                                     {0x6C14DB81, 0x11CEA733, 0x200021A5, 0x60E50BAF},
                                     {0x0B2B8630, 0x11D0AD35, 0x6000A68E, 0x5BEA9797},
                                     {0xBB223240, 0x11D0E72B, 0xAA00B4A9, 0x3E99C000},
                                     {0xB0AB3B60, 0x11D133D7, 0xC00081A9, 0x74B1D74F},
                                     {0x279AFA83, 0x11CE4981, 0x200021A5, 0x60E50BAF},
                                     {0x279AFA85, 0x11CE4981, 0x200021A5, 0x60E50BAF}}};
constexpr Guid kHal{0x84E63DE0, 0x11CF46AA, 0x00006F81, 0x6E1520C0};
constexpr Guid kRgb{0xA4665C60, 0x11CF2673, 0xAA001AA3, 0x5633B900};
constexpr unsigned kSlots[] = {23, 28, 36, 45, 12, 42, 11, 21};
struct HResult {
    std::uint32_t value;
};
void demand(bool condition, std::uint32_t code = kInvalid) {
    if (!condition)
        throw HResult{code};
}
std::uint32_t arg(sh4::Ctx& c, ProcessMemory& m, unsigned i) {
    return argument(c, m, i);
}
Guid guid(ProcessMemory& m, std::uint32_t p) {
    m.check(p, 16, ProcessMemory::Read);
    Guid g{};
    for (unsigned i = 0; i < 4; ++i) g[i] = m.read32(p + i * 4);
    return g;
}
std::string lower(std::string name) {
    for (auto& c : name)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + 32);
    return name;
}
std::uint32_t native_color(std::uint32_t raw, unsigned bits) {
    if (bits == 32)
        return raw & 0xFFFFFF;
    const auto r = (raw >> 11) & 31u, g = (raw >> 5) & 63u, b = raw & 31u;
    return (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | (b << 3) | (b >> 2);
}
}  // namespace

struct DirectX::Impl {
    ProcessMemory& m;
    AudioMixer mixer;
    struct Object {
        ProcessMemory& m;
        AudioMixer& mixer;
        Family family;
        std::uint32_t refs = 1, identity = 0, caps = 0, data = 0, bytes = 0, bits = 32;
        std::map<Kind, std::uint32_t> views;
        std::vector<std::uint32_t> allocations;
        std::shared_ptr<Object> back, target, owner;
        std::unique_ptr<Surface> surface;
        std::unique_ptr<Rasterizer> raster;
        std::shared_ptr<SoundBuffer> sound;
        bool locked = false, readonly = false, scene = false, looping = false, primary = false;
        Rect locked_rect{};
        std::uint32_t lock_pointer = 0, lock_count = 0, lock_second = 0;
        unsigned channels = 2;
        std::uint32_t rate = 44100, frequency = 44100;
        std::int32_t volume = 0;
        std::uint32_t cull = 3;
        Object(ProcessMemory& memory, AudioMixer& audio, Family f)
            : m(memory), mixer(audio), family(f) {}
        ~Object() {
            if (sound)
                mixer.remove(sound);
            for (auto p : allocations) m.release(p);
        }
        std::uint32_t alloc(std::uint32_t n) {
            const auto p = m.allocate(n);
            demand(p != 0, kOutOfMemory);
            try {
                allocations.push_back(p);
            } catch (...) {
                m.release(p);
                throw;
            }
            return p;
        }
    };
    struct View {
        Kind kind;
        std::shared_ptr<Object> object;
    };
    std::map<std::uint32_t, View> views;
    std::uint32_t mode_width = 640, mode_height = 480, mode_bits = 16;
    std::uint32_t frame_width = 0, frame_height = 0;
    std::uint64_t present_count = 0;
    std::vector<std::uint32_t> last_frame;
    std::string diagnostic;
    explicit Impl(ProcessMemory& memory) : m(memory) {}
    std::shared_ptr<Object> create(Family f) {
        demand(views.size() < 1024, kOutOfMemory);
        return std::make_shared<Object>(m, mixer, f);
    }
    std::uint32_t view(const std::shared_ptr<Object>& o, Kind kind) {
        auto it = o->views.find(kind);
        std::uint32_t pointer = 0;
        if (it == o->views.end()) {
            pointer = o->alloc(256);
            m.write32(pointer, pointer + 16);
            const auto index = static_cast<unsigned>(kind);
            for (unsigned i = 0; i < kSlots[index]; ++i)
                m.write32(pointer + 16 + i * 4, kMethods + index * 256 + i * 4);
            o->views.emplace(kind, pointer);
            if (!o->identity)
                o->identity = pointer;
        } else
            pointer = it->second;
        views.insert_or_assign(pointer, View{kind, o});
        return pointer;
    }
    std::shared_ptr<Object> object(std::uint32_t p, Family family) {
        auto it = views.find(p);
        demand(it != views.end() && it->second.object->family == family);
        return it->second.object;
    }
    std::uint32_t release(const std::shared_ptr<Object>& o) {
        demand(o->refs > 0);
        if (--o->refs == 0)
            for (const auto& [kind, p] : o->views) {
                (void)kind;
                views.erase(p);
            }
        return o->refs;
    }
    void output(std::uint32_t p) {
        demand(p != 0 && !(p & 3u), kPointer);
        m.check(p, 4, ProcessMemory::Write);
    }
    std::uint32_t qi(const std::shared_ptr<Object>& o, std::uint32_t iid, std::uint32_t out) {
        output(out);
        const auto g = guid(m, iid);
        m.write32(out, 0);
        if (g == kUnknown) {
            for (const auto& [k, p] : o->views)
                if (p == o->identity) {
                    view(o, k);
                    break;
                }
            m.write32(out, o->identity);
            ++o->refs;
            return 0;
        }
        for (unsigned i = 0; i < kIids.size(); ++i)
            if (g == kIids[i]) {
                const auto k = static_cast<Kind>(i);
                const bool allowed = (o->family == Family::Draw &&
                                      (k == Kind::Draw1 || k == Kind::Draw4 || k == Kind::D3D3)) ||
                                     (o->family == Family::Surface &&
                                      (k == Kind::Surface1 || k == Kind::Surface4)) ||
                                     (o->family == Family::Device && k == Kind::Device3) ||
                                     (o->family == Family::Sound && k == Kind::Sound) ||
                                     (o->family == Family::Buffer && k == Kind::Buffer);
                if (!allowed)
                    return kNoInterface;
                m.write32(out, view(o, k));
                ++o->refs;
                return 0;
            }
        return kNoInterface;
    }
    void from_guest(Object& o) {
        demand(!o.locked, kBusy);
        const auto bytes = m.read_bytes(o.data, o.bytes);
        auto target = o.surface->lock();
        std::copy(bytes.begin(), bytes.end(), target.begin());
        o.surface->unlock();
    }
    void to_guest(Object& o) {
        auto source = o.surface->lock();
        try {
            m.write_bytes(o.data, source);
        } catch (...) {
            o.surface->unlock();
            throw;
        }
        o.surface->unlock();
    }
    Rect rect(std::uint32_t pointer, Object& o) {
        if (!pointer)
            return {0, 0, o.surface->width(), o.surface->height()};
        m.check(pointer, 16, ProcessMemory::Read);
        const auto l = m.read32(pointer), t = m.read32(pointer + 4), r = m.read32(pointer + 8),
                   b = m.read32(pointer + 12);
        demand(l <= r && t <= b && r <= o.surface->width() && b <= o.surface->height());
        return {l, t, r - l, b - t};
    }
    void pixel_format(std::uint32_t p, unsigned bits) {
        m.check(p, 32, ProcessMemory::Write);
        for (unsigned i = 0; i < 8; ++i) m.write32(p + i * 4, 0);
        m.write32(p, 32);
        m.write32(p + 4, 0x40);
        m.write32(p + 12, bits);
        m.write32(p + 16, bits == 16 ? 0xF800 : 0xFF0000);
        m.write32(p + 20, bits == 16 ? 0x7E0 : 0xFF00);
        m.write32(p + 24, bits == 16 ? 0x1F : 0xFF);
    }
    void description(std::uint32_t p, Object& o, Kind kind, std::uint32_t pixels = 0) {
        const auto size = kind == Kind::Surface1 || kind == Kind::Draw1 ? 108u : 124u;
        demand(m.read32(p) == size);
        m.check(p, size, ProcessMemory::Write);
        for (unsigned i = 4; i < size; i += 4) m.write32(p + i, 0);
        m.write32(p + 4, 0x100F | (pixels ? 0x800u : 0u));
        m.write32(p + 8, o.surface->height());
        m.write32(p + 12, o.surface->width());
        m.write32(p + 16, o.surface->pitch());
        m.write32(p + 36, pixels);
        pixel_format(p + 72, o.bits);
        m.write32(p + 104, o.caps);
    }
    std::shared_ptr<Object> surface(std::uint32_t w, std::uint32_t h, unsigned bits,
                                    std::uint32_t caps) {
        demand(w && h && w <= 2048 && h <= 2048 && (bits == 16 || bits == 32));
        auto o = create(Family::Surface);
        o->bits = bits;
        o->caps = caps;
        o->surface = std::make_unique<Surface>(
            w, h, bits == 16 ? PixelFormat::Rgb565 : PixelFormat::Xrgb8888);
        o->bytes = o->surface->pitch() * h;
        o->data = o->alloc(o->bytes);
        return o;
    }
    void present(Object& o) {
        from_guest(o);
        last_frame = o.surface->rgba();
        frame_width = o.surface->width();
        frame_height = o.surface->height();
        ++present_count;
    }
    std::uint32_t draw(const std::shared_ptr<Object>& o, Kind kind, unsigned slot, sh4::Ctx& c);
    std::uint32_t surf(const std::shared_ptr<Object>& o, Kind kind, unsigned slot, sh4::Ctx& c);
    std::uint32_t d3d(const std::shared_ptr<Object>& o, unsigned slot, sh4::Ctx& c);
    std::uint32_t device(const std::shared_ptr<Object>& o, unsigned slot, sh4::Ctx& c);
    std::uint32_t sound(const std::shared_ptr<Object>& o, unsigned slot, sh4::Ctx& c);
    std::uint32_t buffer(const std::shared_ptr<Object>& o, unsigned slot, sh4::Ctx& c);
    std::uint32_t unsupported() { throw HResult{kUnsupported}; }
};

std::uint32_t DirectX::Impl::draw(const std::shared_ptr<Object>& o, Kind kind, unsigned slot,
                                  sh4::Ctx& c) {
    auto a = [&](unsigned i) { return arg(c, m, i); };
    switch (slot) {
        case 6: {  // CreateSurface
            output(a(2));
            demand(a(3) == 0, kNoAggregation);
            const auto p = a(1), size = kind == Kind::Draw1 ? 108u : 124u;
            m.check(p, size, ProcessMemory::Read);
            demand(m.read32(p) == size);
            const auto flags = m.read32(p + 4), caps = m.read32(p + 104);
            demand((flags & 1) && !(flags & ~(1u | 2u | 4u | 0x20u | 0x1000u)), kUnsupported);
            demand(!(caps & ~(0x200u | 0x40u | 0x800u | 0x4000u | 0x2000u | 8u | 16u | 4u)),
                   kUnsupported);
            demand(size == 108 || !(m.read32(p + 108) | m.read32(p + 112) | m.read32(p + 116) |
                                    m.read32(p + 120)),
                   kUnsupported);
            demand((caps & 0x4800) != 0x4800);
            const bool primary = (caps & 0x200) != 0;
            demand(primary || (flags & 6) == 6);
            const auto w = primary ? mode_width : m.read32(p + 12),
                       h = primary ? mode_height : m.read32(p + 8);
            unsigned bits = mode_bits;
            if (flags & 0x1000) {
                demand(m.read32(p + 72) == 32 && m.read32(p + 76) == 0x40, kUnsupported);
                bits = m.read32(p + 84);
                demand(bits == 16 || bits == 32, kUnsupported);
                demand(m.read32(p + 88) == (bits == 16 ? 0xF800u : 0xFF0000u) &&
                           m.read32(p + 92) == (bits == 16 ? 0x7E0u : 0xFF00u) &&
                           m.read32(p + 96) == (bits == 16 ? 0x1Fu : 0xFFu) &&
                           m.read32(p + 100) == 0,
                       kUnsupported);
            }
            const bool flip = (caps & 16) != 0;
            demand(flip || !(caps & 8), kUnsupported);
            demand(flip ? primary && (caps & 8) && (flags & 0x20) && m.read32(p + 20) == 1
                        : !(flags & 0x20),
                   kUnsupported);
            auto s = surface(w, h, bits, caps);
            s->owner = o;
            if (flip) {
                s->back = surface(w, h, bits, 4 | 0x4000 | (caps & 0x2000));
                s->back->owner = o;
                s->back->refs = 0;
            }
            m.write32(a(2), view(s, kind == Kind::Draw1 ? Kind::Surface1 : Kind::Surface4));
            return 0;
        }
        case 20:
            demand(a(2) == 8 || a(2) == 0x11, kUnsupported);
            return 0;  // virtual cooperative level
        case 21:
            demand(a(1) && a(1) <= 2048 && a(2) && a(2) <= 2048 && (a(3) == 16 || a(3) == 32));
            if (kind == Kind::Draw4)
                demand((a(4) == 0 || a(4) == 60) && a(5) == 0, kUnsupported);
            mode_width = a(1);
            mode_height = a(2);
            mode_bits = a(3);
            return 0;
        case 19:
            mode_width = 640;
            mode_height = 480;
            mode_bits = 16;
            return 0;
        default:
            return unsupported();
    }
}

std::uint32_t DirectX::Impl::surf(const std::shared_ptr<Object>& o, Kind kind, unsigned slot,
                                  sh4::Ctx& c) {
    auto a = [&](unsigned i) { return arg(c, m, i); };
    switch (slot) {
        case 5: {  // Blt
            const auto dst = rect(a(1), *o);
            const auto flags = a(4);
            demand(!(flags & ~(0x1000000u | 0x400u)), kUnsupported);
            from_guest(*o);
            if (flags & 0x400) {
                demand(a(2) == 0 && a(5) != 0);
                m.check(a(5), 100, ProcessMemory::Read);
                demand(m.read32(a(5)) == 100);
                o->surface->fill(dst, native_color(m.read32(a(5) + 80), o->bits));
            } else {
                auto source = object(a(2), Family::Surface);
                const auto src = rect(a(3), *source);
                demand(src.width == dst.width && src.height == dst.height, kUnsupported);
                from_guest(*source);
                o->surface->blit(dst.x, dst.y, *source->surface, src);
            }
            to_guest(*o);
            if (o->caps & 0x200)
                present(*o);
            return 0;
        }
        case 7: {  // BltFast, no color key yet
            demand((a(5) & ~0x10u) == 0, kUnsupported);
            auto source = object(a(3), Family::Surface);
            const auto src = rect(a(4), *source);
            from_guest(*source);
            from_guest(*o);
            o->surface->blit(a(1), a(2), *source->surface, src);
            to_guest(*o);
            if (o->caps & 0x200)
                present(*o);
            return 0;
        }
        case 11: {  // Flip
            demand(o->back != nullptr && (a(1) == 0 || object(a(1), Family::Surface) == o->back));
            demand(!(a(2) & ~1u), kUnsupported);
            from_guest(*o);
            from_guest(*o->back);
            const auto front = m.read_bytes(o->data, o->bytes),
                       back = m.read_bytes(o->back->data, o->back->bytes);
            m.write_bytes(o->data, back);
            m.write_bytes(o->back->data, front);
            present(*o);
            return 0;
        }
        case 12:  // GetAttachedSurface
            output(a(2));
            demand(o->back && m.read32(a(1)) == 4, kUnsupported);
            m.write32(a(2), view(o->back, kind));
            ++o->back->refs;
            return 0;
        case 14:
            m.check(a(1), kind == Kind::Surface1 ? 4u : 16u, ProcessMemory::Write);
            m.write32(a(1), o->caps);
            if (kind == Kind::Surface4)
                for (unsigned n = 4; n < 16; n += 4) m.write32(a(1) + n, 0);
            return 0;
        case 21:
            pixel_format(a(1), o->bits);
            return 0;
        case 22:
            description(a(1), *o, kind);
            return 0;
        case 24:
            return 0;  // IsLost: CPU surfaces cannot be lost
        case 25: {     // Lock
            demand(!o->locked, kBusy);
            demand(!(a(3) & ~(1u | 0x10u | 0x20u)) && !a(4), kUnsupported);
            demand((a(3) & 0x30) != 0x30);
            const auto r = rect(a(1), *o);
            demand(r.width && r.height);
            const auto pointer = o->data + r.y * o->surface->pitch() + r.x * (o->bits / 8);
            description(a(2), *o, kind, pointer);
            from_guest(*o);  // Keep a snapshot for a read-only lock, including after a flip.
            o->locked = true;
            o->readonly = (a(3) & 0x10) != 0;
            o->locked_rect = r;
            o->lock_pointer = pointer;
            return 0;
        }
        case 32: {  // Unlock
            demand(o->locked);
            if (kind == Kind::Surface1)
                demand(a(1) == 0 || a(1) == o->lock_pointer);
            else if (a(1)) {
                const auto r = rect(a(1), *o);
                demand(r.x == o->locked_rect.x && r.y == o->locked_rect.y &&
                       r.width == o->locked_rect.width && r.height == o->locked_rect.height);
            }
            o->locked = false;
            if (!o->readonly)
                from_guest(*o);
            else
                to_guest(*o);
            if (o->caps & 0x200)
                present(*o);
            return 0;
        }
        default:
            return unsupported();
    }
}

std::uint32_t DirectX::Impl::d3d(const std::shared_ptr<Object>& o, unsigned slot, sh4::Ctx& c) {
    auto a = [&](unsigned i) { return arg(c, m, i); };
    if (slot != 8)
        return unsupported();
    output(a(3));
    demand(!a(4), kNoAggregation);
    const auto id = guid(m, a(1));
    demand(id == kHal || id == kRgb, kUnsupported);
    auto target = object(a(2), Family::Surface);
    demand((target->caps & 0x2000) != 0);
    auto d = create(Family::Device);
    d->target = target;
    d->owner = o;
    d->raster = std::make_unique<Rasterizer>(*target->surface);
    d->raster->depth_enabled(false);
    m.write32(a(3), view(d, Kind::Device3));
    return 0;
}
std::uint32_t DirectX::Impl::device(const std::shared_ptr<Object>& o, unsigned slot, sh4::Ctx& c) {
    auto a = [&](unsigned i) { return arg(c, m, i); };
    switch (slot) {
        case 9:
            demand(!o->scene);
            o->scene = true;
            return 0;
        case 10:
            demand(o->scene);
            o->scene = false;
            return 0;
        case 11:
            output(a(1));
            m.write32(a(1), view(o->owner, Kind::D3D3));
            ++o->owner->refs;
            return 0;
        case 15:
            output(a(1));
            m.write32(a(1), view(o->target, Kind::Surface4));
            ++o->target->refs;
            return 0;
        case 22:  // SetRenderState: only behavior actually supplied by this renderer
            if (a(1) == 22) {
                demand(a(2) >= 1 && a(2) <= 3);
                o->cull = a(2);
                return 0;
            }
            demand((a(1) == 7 && a(2) == 0) || (a(1) == 8 && a(2) == 3) ||
                       (a(1) == 9 && a(2) == 2) ||
                       ((a(1) == 27 || a(1) == 28 || a(1) == 29) && a(2) == 0),
                   kUnsupported);
            return 0;
        case 28:
        case 29: {  // Draw[Indexed]Primitive
            demand(o->scene);
            demand(a(2) == 0x1C4, kUnsupported);  // D3DFVF_TLVERTEX, 32-byte vertices
            const auto type = a(1), count = a(4);
            const bool indexed = slot == 29;
            demand(type >= 4 && type <= 6, kUnsupported);
            demand(count && count <= 65536);
            const auto n = indexed ? a(6) : count;
            demand(n && n <= 65536 && (type != 4 || n % 3 == 0) && n >= 3);
            demand(a(indexed ? 7 : 5) == 0, kUnsupported);
            m.check(a(3), count * 32, ProcessMemory::Read);
            if (indexed)
                m.check(a(5), n * 2, ProcessMemory::Read);
            std::vector<TlVertex> vertices;
            vertices.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                const auto index = indexed ? m.read16(a(5) + i * 2) : i;
                demand(index < count);
                const auto p = a(3) + index * 32;
                TlVertex v{std::bit_cast<float>(m.read32(p)), std::bit_cast<float>(m.read32(p + 4)),
                           std::bit_cast<float>(m.read32(p + 8)), m.read32(p + 16)};
                demand(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
                       std::abs(v.x) <= 1000000 && std::abs(v.y) <= 1000000 && v.z >= 0 &&
                       v.z <= 1);
                vertices.push_back(v);
            }
            from_guest(*o->target);
            auto triangle = [&](TlVertex x, TlVertex y, TlVertex z) {
                const double winding = (static_cast<double>(y.x) - x.x) * (z.y - x.y) -
                                       (static_cast<double>(y.y) - x.y) * (z.x - x.x);
                if ((o->cull == 2 && winding > 0) || (o->cull == 3 && winding < 0))
                    return;
                o->raster->triangle(x, y, z);
            };
            if (type == 4)
                for (unsigned i = 0; i < n; i += 3)
                    triangle(vertices[i], vertices[i + 1], vertices[i + 2]);
            else
                for (unsigned i = 2; i < n; ++i) {
                    if (type == 6)
                        triangle(vertices[0], vertices[i - 1], vertices[i]);
                    else if (i & 1)
                        triangle(vertices[i - 1], vertices[i - 2], vertices[i]);
                    else
                        triangle(vertices[i - 2], vertices[i - 1], vertices[i]);
                }
            to_guest(*o->target);
            return 0;
        }
        case 38:
            demand(a(1) == 0 && a(2) == 0, kUnsupported);
            return 0;  // SetTexture(NULL)
        default:
            return unsupported();
    }
}

std::uint32_t DirectX::Impl::sound(const std::shared_ptr<Object>& o, unsigned slot, sh4::Ctx& c) {
    auto a = [&](unsigned i) { return arg(c, m, i); };
    if (slot == 6) {
        demand(a(2) >= 1 && a(2) <= 3, kUnsupported);
        return 0;
    }
    if (slot != 3)
        return unsupported();
    output(a(2));
    demand(a(3) == 0, kNoAggregation);
    const auto desc = a(1), size = m.read32(desc);
    demand(size == 20 || size == 36);
    m.check(desc, size, ProcessMemory::Read);
    const auto caps = m.read32(desc + 4), bytes = m.read32(desc + 8), format = m.read32(desc + 16);
    demand(m.read32(desc + 12) == 0);
    demand(!(caps & ~(1u | 2u | 8u | 0x20u | 0x80u | 0x4000u | 0x8000u | 0x10000u)), kUnsupported);
    auto b = create(Family::Buffer);
    b->owner = o;
    b->caps = caps;
    b->primary = (caps & 1) != 0;
    if (b->primary)
        demand(bytes == 0 && format == 0);
    else {
        m.check(format, 18, ProcessMemory::Read);
        demand(m.read16(format) == 1 && m.read16(format + 16) == 0, 0x88780064);
        b->channels = m.read16(format + 2);
        b->rate = m.read32(format + 4);
        b->frequency = b->rate;
        b->bits = m.read16(format + 14);
        demand((b->channels == 1 || b->channels == 2) && (b->bits == 8 || b->bits == 16) &&
                   b->rate >= 8000 && b->rate <= 192000,
               0x88780064);
        const auto align = b->channels * b->bits / 8;
        demand(m.read16(format + 12) == align && m.read32(format + 8) == b->rate * align,
               0x88780064);
        demand(bytes && bytes <= (16u << 20) && bytes % align == 0);
        b->bytes = bytes;
        b->data = b->alloc(bytes);
        b->sound = std::make_shared<SoundBuffer>(bytes / align, b->channels, b->bits, b->rate);
        if (b->bits == 8)
            m.write_bytes(b->data, std::vector<std::uint8_t>(bytes, 128));
        mixer.add(b->sound);
    }
    m.write32(a(2), view(b, Kind::Buffer));
    return 0;
}
std::uint32_t DirectX::Impl::buffer(const std::shared_ptr<Object>& o, unsigned slot, sh4::Ctx& c) {
    auto a = [&](unsigned i) { return arg(c, m, i); };
    if (slot == 13) {  // SetFormat on the primary mix format
        demand(o->primary, 0x88780032);
        m.check(a(1), 18, ProcessMemory::Read);
        demand(m.read16(a(1)) == 1 && m.read16(a(1) + 2) == 2 && m.read32(a(1) + 4) == 44100 &&
                   m.read32(a(1) + 8) == 176400 && m.read16(a(1) + 12) == 4 &&
                   m.read16(a(1) + 14) == 16 && m.read16(a(1) + 16) == 0,
               0x88780064);
        return 0;
    }
    demand(!o->primary, kUnsupported);
    const auto align = o->channels * o->bits / 8;
    switch (slot) {
        case 4: {
            if (a(1))
                output(a(1));
            if (a(2))
                output(a(2));
            const auto cursor = o->sound->cursor() * align;
            if (a(1))
                m.write32(a(1), cursor);
            if (a(2))
                m.write32(a(2),
                          cursor);  // deterministic software mixer, no queued host-device latency
            return 0;
        }
        case 6:
            output(a(1));
            demand(o->caps & 0x80, 0x8878001E);
            m.write32(a(1), static_cast<std::uint32_t>(o->volume));
            return 0;
        case 8:
            output(a(1));
            m.write32(a(1), o->frequency);
            return 0;
        case 9:
            output(a(1));
            m.write32(a(1), o->sound->playing() ? 1u | (o->looping ? 4u : 0u) : 0u);
            return 0;
        case 11: {  // Lock returns one or two guest spans, including arguments on the SH-4 stack
            demand(!o->locked, 0x88780032);
            const auto flags = a(7);
            demand((flags & ~2u) == 0, kUnsupported);
            const auto offset = flags & 2 ? 0u : a(1), count = flags & 2 ? o->bytes : a(2);
            demand(offset < o->bytes && count && count <= o->bytes);
            const auto first = std::min(count, o->bytes - offset), second = count - first;
            output(a(3));
            output(a(4));
            demand((a(5) == 0) == (a(6) == 0));
            if (second)
                demand(a(5) != 0);
            if (a(5)) {
                output(a(5));
                output(a(6));
            }
            m.write32(a(3), o->data + offset);
            m.write32(a(4), first);
            if (a(5)) {
                m.write32(a(5), second ? o->data : 0u);
                m.write32(a(6), second);
            }
            o->locked = true;
            o->lock_pointer = o->data + offset;
            o->lock_count = first;
            o->lock_second = second;
            return 0;
        }
        case 12:
            demand(!o->locked, 0x88780032);
            demand(a(1) == 0 && a(2) == 0 && !(a(3) & ~1u), kUnsupported);
            o->sound->write(0, m.read_bytes(o->data, o->bytes));
            o->looping = (a(3) & 1) != 0;
            o->sound->play(o->looping);
            return 0;
        case 14:
            demand(a(1) < o->bytes && a(1) % align == 0);
            o->sound->seek(a(1) / align);
            return 0;
        case 15: {
            demand(o->caps & 0x80, 0x8878001E);
            const auto v = static_cast<std::int32_t>(a(1));
            demand(v >= -10000 && v <= 0);
            o->sound->gain(v == -10000 ? 0u
                                       : static_cast<std::uint32_t>(
                                             std::lround(32768 * std::pow(10.0, v / 2000.0))));
            o->volume = v;
            return 0;
        }
        case 17:
            demand(o->caps & 0x20, 0x8878001E);
            demand(!a(1) || (a(1) >= 100 && a(1) <= 200000));
            o->frequency = a(1) ? a(1) : o->rate;
            o->sound->frequency(o->frequency);
            return 0;
        case 18:
            o->sound->stop();
            return 0;
        case 19:
            demand(o->locked && a(1) == o->lock_pointer && a(2) == o->lock_count &&
                   a(3) == (o->lock_second ? o->data : 0u) && a(4) == o->lock_second);
            o->sound->write(a(1) - o->data, m.read_bytes(a(1), a(2)));
            if (a(4))
                o->sound->write(0, m.read_bytes(a(3), a(4)));
            o->locked = false;
            return 0;
        default:
            return unsupported();
    }
}

DirectX::DirectX(ProcessMemory& m) : impl_(std::make_unique<Impl>(m)) {}
DirectX::~DirectX() = default;
std::optional<std::uint32_t> DirectX::resolve(const Import& s) const {
    if (s.ordinal)
        return {};
    const auto dll = lower(s.dll);
    if (dll == "ddraw.dll" && s.name == "DirectDrawCreate")
        return kFactory;
    if (dll == "ddraw.dll" && s.name == "DirectDrawCreateEx")
        return kFactory + 4;
    if (dll == "dsound.dll" && s.name == "DirectSoundCreate")
        return kFactory + 8;
    return {};
}
bool DirectX::invoke(sh4::Ctx& c, Memory& memory, std::uint32_t target) {
    const bool factory = target >= kFactory && target < kFactory + 12 && !(target & 3);
    const bool method = target >= kMethods && target < kMethods + 8 * 256 && !(target & 3);
    if (!factory && !method)
        return false;
    auto& i = *impl_;
    if (&memory != &i.m)
        throw std::invalid_argument("DirectX called with a different guest address space");
    try {
        if (factory) {
            const auto index = (target - kFactory) / 4;
            i.output(c.r[5]);
            i.m.write32(c.r[5], 0);
            demand(c.r[4] == 0, kUnsupported);
            demand(c.r[index == 1 ? 7 : 6] == 0, kNoAggregation);
            if (index == 1)
                demand(guid(i.m, c.r[6]) == kIids[1], kNoInterface);
            auto o = i.create(index == 2 ? Family::Sound : Family::Draw);
            i.m.write32(c.r[5], i.view(o, index == 2   ? Kind::Sound
                                          : index == 1 ? Kind::Draw4
                                                       : Kind::Draw1));
            c.r[0] = 0;
        } else {
            const auto kind = static_cast<Kind>((target - kMethods) / 256);
            const auto slot = ((target - kMethods) % 256) / 4;
            const auto it = i.views.find(c.r[4]);
            demand(it != i.views.end() && it->second.kind == kind &&
                   slot < kSlots[static_cast<unsigned>(kind)]);
            const auto o = it->second.object;
            if (slot == 0)
                c.r[0] = i.qi(o, c.r[5], c.r[6]);
            else if (slot == 1) {
                demand(o->refs < 65535);
                c.r[0] = ++o->refs;
            } else if (slot == 2)
                c.r[0] = i.release(o);
            else
                switch (kind) {
                    case Kind::Draw1:
                    case Kind::Draw4:
                        c.r[0] = i.draw(o, kind, slot, c);
                        break;
                    case Kind::Surface1:
                    case Kind::Surface4:
                        c.r[0] = i.surf(o, kind, slot, c);
                        break;
                    case Kind::D3D3:
                        c.r[0] = i.d3d(o, slot, c);
                        break;
                    case Kind::Device3:
                        c.r[0] = i.device(o, slot, c);
                        break;
                    case Kind::Sound:
                        c.r[0] = i.sound(o, slot, c);
                        break;
                    case Kind::Buffer:
                        c.r[0] = i.buffer(o, slot, c);
                        break;
                }
        }
    } catch (const HResult& e) {
        c.r[0] = e.value;
    } catch (const std::bad_alloc&) {
        c.r[0] = kOutOfMemory;
    } catch (const std::out_of_range&) {
        c.r[0] = kInvalid;
    } catch (const std::invalid_argument&) {
        c.r[0] = kInvalid;
    } catch (const std::runtime_error& e) {
        c.r[0] = kPointer;
        i.diagnostic = e.what();
    }
    if (c.r[0] & 0x80000000u)
        i.diagnostic = "DirectX call at " + std::to_string(target) + " failed: HRESULT " +
                       std::to_string(c.r[0]);
    return true;
}
void DirectX::mix(std::span<std::int16_t> stereo) {
    impl_->mixer.mix(stereo);
}
const std::vector<std::uint32_t>& DirectX::frame() const {
    return impl_->last_frame;
}
std::uint32_t DirectX::width() const {
    return impl_->frame_width;
}
std::uint32_t DirectX::height() const {
    return impl_->frame_height;
}
std::uint64_t DirectX::presents() const {
    return impl_->present_count;
}
const std::string& DirectX::diagnostic() const {
    return impl_->diagnostic;
}
}  // namespace dream::wince
