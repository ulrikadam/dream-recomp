#include "dream/runtime/wince/media.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace dream::wince {
Surface::Surface(std::uint32_t width, std::uint32_t height, PixelFormat format)
    : width_(width),
      height_(height),
      pitch_(0),
      bpp_(format == PixelFormat::Rgb565 ? 2u : 4u),
      format_(format) {
    if (!width || !height || width > 2048 || height > 2048)
        throw std::invalid_argument("invalid surface dimensions");
    if (format != PixelFormat::Rgb565 && format != PixelFormat::Xrgb8888)
        throw std::invalid_argument("invalid pixel format");
    pitch_ = (width * bpp_ + 3u) & ~3u;
    bytes_.resize(static_cast<std::size_t>(pitch_) * height);
}
void Surface::check(Rect r) const {
    if (locked_)
        throw std::runtime_error("surface is locked");
    if (r.x > width_ || r.y > height_ || r.width > width_ - r.x || r.height > height_ - r.y)
        throw std::out_of_range("surface rectangle out of bounds");
}
std::span<std::uint8_t> Surface::lock() {
    if (locked_)
        throw std::runtime_error("surface already locked");
    locked_ = true;
    return bytes_;
}
void Surface::unlock() {
    if (!locked_)
        throw std::runtime_error("surface is not locked");
    locked_ = false;
}
std::uint32_t Surface::pixel(std::uint32_t x, std::uint32_t y) const {
    check({x, y, 1, 1});
    const auto p = static_cast<std::size_t>(y) * pitch_ + x * bpp_;
    if (format_ == PixelFormat::Xrgb8888)
        return 0xFF000000u | bytes_[p] | (static_cast<std::uint32_t>(bytes_[p + 1]) << 8) |
               (static_cast<std::uint32_t>(bytes_[p + 2]) << 16);
    const std::uint32_t raw = bytes_[p] | (static_cast<std::uint32_t>(bytes_[p + 1]) << 8);
    const auto r = (raw >> 11) & 31u, g = (raw >> 5) & 63u, b = raw & 31u;
    return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | (b << 3) |
           (b >> 2);
}
void Surface::pixel(std::uint32_t x, std::uint32_t y, std::uint32_t rgb) {
    check({x, y, 1, 1});
    const auto p = static_cast<std::size_t>(y) * pitch_ + x * bpp_;
    const auto packed = format_ == PixelFormat::Rgb565
                            ? ((rgb >> 8) & 0xF800u) | ((rgb >> 5) & 0x7E0u) | ((rgb >> 3) & 31u)
                            : rgb;
    for (unsigned i = 0; i < bpp_; ++i)
        bytes_[p + i] = static_cast<std::uint8_t>(packed >> (8 * i));
}
void Surface::fill(Rect r, std::uint32_t rgb) {
    check(r);
    for (std::uint32_t y = r.y; y < r.y + r.height; ++y)
        for (std::uint32_t x = r.x; x < r.x + r.width; ++x) pixel(x, y, rgb);
}
void Surface::blit(std::uint32_t x, std::uint32_t y, const Surface& source, Rect r,
                   std::optional<std::uint32_t> key) {
    check({x, y, r.width, r.height});
    source.check(r);
    // Snapshot makes overlapping copies on the same surface deterministic.
    std::vector<std::uint32_t> copy;
    copy.reserve(static_cast<std::size_t>(r.width) * r.height);
    for (std::uint32_t sy = 0; sy < r.height; ++sy)
        for (std::uint32_t sx = 0; sx < r.width; ++sx)
            copy.push_back(source.pixel(r.x + sx, r.y + sy));
    for (std::uint32_t dy = 0; dy < r.height; ++dy)
        for (std::uint32_t dx = 0; dx < r.width; ++dx) {
            const auto color = copy[static_cast<std::size_t>(dy) * r.width + dx];
            if (!key || (color & 0xFFFFFFu) != (*key & 0xFFFFFFu))
                pixel(x + dx, y + dy, color);
        }
}
std::vector<std::uint32_t> Surface::rgba() const {
    check({0, 0, width_, height_});
    std::vector<std::uint32_t> out;
    out.reserve(static_cast<std::size_t>(width_) * height_);
    for (std::uint32_t y = 0; y < height_; ++y)
        for (std::uint32_t x = 0; x < width_; ++x) out.push_back(pixel(x, y));
    return out;
}

Rasterizer::Rasterizer(Surface& target)
    : target_(target),
      depth_(static_cast<std::size_t>(target.width()) * target.height(),
             std::numeric_limits<float>::infinity()) {}
void Rasterizer::clear_depth() {
    std::fill(depth_.begin(), depth_.end(), std::numeric_limits<float>::infinity());
}
void Rasterizer::triangle(TlVertex a, TlVertex b, TlVertex c) {
    if (target_.locked())
        throw std::runtime_error("triangle target is locked");
    auto valid = [](const TlVertex& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
               std::abs(v.x) <= 1000000 && std::abs(v.y) <= 1000000 && v.z >= 0 && v.z <= 1;
    };
    if (!valid(a) || !valid(b) || !valid(c))
        throw std::invalid_argument("invalid pretransformed vertex");
    auto edge = [](const TlVertex& u, const TlVertex& v, double x, double y) {
        return (static_cast<double>(v.x) - u.x) * (y - u.y) -
               (static_cast<double>(v.y) - u.y) * (x - u.x);
    };
    double area = edge(a, b, c.x, c.y);
    if (area == 0)
        return;
    if (area < 0) {
        std::swap(b, c);
        area = -area;
    }
    const auto min_x = static_cast<int>(std::max(0.0f, std::floor(std::min({a.x, b.x, c.x}))));
    const auto min_y = static_cast<int>(std::max(0.0f, std::floor(std::min({a.y, b.y, c.y}))));
    const auto max_x = static_cast<int>(
        std::min(static_cast<float>(target_.width()), std::ceil(std::max({a.x, b.x, c.x}))));
    const auto max_y = static_cast<int>(
        std::min(static_cast<float>(target_.height()), std::ceil(std::max({a.y, b.y, c.y}))));
    auto top_left = [](const TlVertex& u, const TlVertex& v) {
        return v.y < u.y || (v.y == u.y && v.x > u.x);
    };
    for (int y = min_y; y < max_y; ++y)
        for (int x = min_x; x < max_x; ++x) {
            const auto ea = edge(b, c, x + 0.5, y + 0.5), eb = edge(c, a, x + 0.5, y + 0.5),
                       ec = edge(a, b, x + 0.5, y + 0.5);
            if (ea < 0 || eb < 0 || ec < 0 || (ea == 0 && !top_left(b, c)) ||
                (eb == 0 && !top_left(c, a)) || (ec == 0 && !top_left(a, b)))
                continue;
            const auto wa = ea / area, wb = eb / area, wc = ec / area;
            const auto z = static_cast<float>(wa * a.z + wb * b.z + wc * c.z);
            const auto index =
                static_cast<std::size_t>(y) * target_.width() + static_cast<unsigned>(x);
            if (depth_enabled_ && z >= depth_[index])
                continue;
            std::uint32_t color = 0xFF000000u;
            for (unsigned shift = 0; shift < 24; shift += 8) {
                const auto value = wa * ((a.color >> shift) & 255u) +
                                   wb * ((b.color >> shift) & 255u) +
                                   wc * ((c.color >> shift) & 255u);
                color |= static_cast<std::uint32_t>(std::clamp(std::lround(value), 0l, 255l))
                         << shift;
            }
            target_.pixel(static_cast<unsigned>(x), static_cast<unsigned>(y), color);
            if (depth_enabled_)
                depth_[index] = z;
        }
}

SoundBuffer::SoundBuffer(std::uint32_t frames, unsigned channels, unsigned bits, std::uint32_t rate)
    : frames_(frames), rate_(rate), channels_(channels), bits_(bits) {
    if (!frames || frames > (1u << 22) || (channels != 1 && channels != 2) ||
        (bits != 8 && bits != 16) || rate < 8000 || rate > 192000)
        throw std::invalid_argument("unsupported PCM buffer format");
    pcm_.resize(static_cast<std::size_t>(frames) * channels * (bits / 8), bits == 8 ? 128u : 0u);
}
void SoundBuffer::write(std::uint32_t offset, std::span<const std::uint8_t> pcm) {
    if (offset >= pcm_.size() || pcm.size() > pcm_.size())
        throw std::out_of_range("PCM write outside buffer");
    // A DirectSound lock can wrap and returns two spans; this primitive consumes their
    // concatenation.
    for (std::size_t i = 0; i < pcm.size(); ++i) pcm_[(offset + i) % pcm_.size()] = pcm[i];
}
void SoundBuffer::seek(std::uint32_t frame) {
    if (frame >= frames_)
        throw std::out_of_range("PCM cursor outside buffer");
    position_ = static_cast<std::uint64_t>(frame) * 44100;
}
void SoundBuffer::gain(std::uint32_t q15) {
    if (q15 > 32768)
        throw std::invalid_argument("PCM gain outside [0,32768]");
    gain_ = q15;
}
void SoundBuffer::frequency(std::uint32_t hz) {
    if (hz < 100 || hz > 200000)
        throw std::invalid_argument("PCM frequency outside [100,200000]");
    rate_ = hz;
}
std::uint32_t SoundBuffer::cursor() const {
    return static_cast<std::uint32_t>(position_ / 44100);
}
std::int32_t SoundBuffer::sample(std::uint32_t frame, unsigned channel) const {
    const auto p = (static_cast<std::size_t>(frame) * channels_ + (channels_ == 1 ? 0 : channel)) *
                   (bits_ / 8);
    if (bits_ == 8)
        return (static_cast<std::int32_t>(pcm_[p]) - 128) * 256;
    const auto raw =
        static_cast<std::uint16_t>(pcm_[p] | (static_cast<unsigned>(pcm_[p + 1]) << 8));
    return static_cast<std::int16_t>(raw);
}
void SoundBuffer::accumulate(std::span<std::int64_t> stereo) {
    const auto end = static_cast<std::uint64_t>(frames_) * 44100;
    for (std::size_t i = 0; i < stereo.size() && playing_; i += 2) {
        stereo[i] += static_cast<std::int64_t>(sample(cursor(), 0)) * gain_ / 32768;
        stereo[i + 1] += static_cast<std::int64_t>(sample(cursor(), 1)) * gain_ / 32768;
        position_ += rate_;
        if (position_ >= end) {
            if (looping_)
                position_ %= end;
            else {
                position_ = 0;
                playing_ = false;
            }
        }
    }
}
void AudioMixer::add(std::shared_ptr<SoundBuffer> buffer) {
    if (!buffer)
        throw std::invalid_argument("null sound buffer");
    if (std::find(buffers_.begin(), buffers_.end(), buffer) != buffers_.end())
        return;
    if (buffers_.size() >= 256)
        throw std::runtime_error("too many sound buffers");
    buffers_.push_back(std::move(buffer));
}
void AudioMixer::remove(const std::shared_ptr<SoundBuffer>& buffer) {
    std::erase(buffers_, buffer);
}
void AudioMixer::mix(std::span<std::int16_t> stereo) {
    if ((stereo.size() & 1u) || stereo.size() > 44100u * 2)
        throw std::invalid_argument("invalid stereo mix block");
    std::vector<std::int64_t> accum(stereo.size());
    for (const auto& buffer : buffers_) buffer->accumulate(accum);
    for (std::size_t i = 0; i < stereo.size(); ++i)
        stereo[i] = static_cast<std::int16_t>(
            std::clamp(accum[i], std::int64_t{-32768}, std::int64_t{32767}));
}
}  // namespace dream::wince
