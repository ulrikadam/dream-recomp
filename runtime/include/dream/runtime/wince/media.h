#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace dream::wince {
// CPU backend primitives used by the CE DirectX bridge in directx.h. Guest structures and
// interface dispatch are kept in that bridge, independent of these host-side objects.
struct Rect {
    std::uint32_t x, y, width, height;
};
enum class PixelFormat { Rgb565, Xrgb8888 };
class Surface {
public:
    Surface(std::uint32_t width, std::uint32_t height, PixelFormat format);
    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
    std::uint32_t pitch() const { return pitch_; }
    std::span<std::uint8_t> lock();
    void unlock();
    void fill(Rect rect, std::uint32_t rgb);
    void blit(std::uint32_t x, std::uint32_t y, const Surface& source, Rect rect,
              std::optional<std::uint32_t> source_key = {});
    std::vector<std::uint32_t> rgba() const;  // 0xFFRRGGBB for a host presentation adapter
    std::uint32_t pixel(std::uint32_t x, std::uint32_t y) const;
    void pixel(std::uint32_t x, std::uint32_t y, std::uint32_t rgb);
    bool locked() const { return locked_; }

private:
    void check(Rect rect) const;
    std::uint32_t width_, height_, pitch_, bpp_;
    PixelFormat format_;
    bool locked_ = false;
    std::vector<std::uint8_t> bytes_;
};

struct TlVertex {
    float x, y, z;
    std::uint32_t color;
};
class Rasterizer {
public:
    explicit Rasterizer(Surface& target);
    void clear_depth();
    void depth_enabled(bool enabled) { depth_enabled_ = enabled; }
    // Pretransformed, opaque, Gouraud-shaded triangle with a [0,1] depth test.
    // No textures, perspective transform, blending or lighting.
    void triangle(TlVertex a, TlVertex b, TlVertex c);

private:
    Surface& target_;
    std::vector<float> depth_;
    bool depth_enabled_ = true;
};

// PCM storage/mixing backend for the DirectSound bridge. Samples are mixed to stereo
// signed 16-bit, 44.1 kHz; the caller supplies the host audio sink. No host audio thread here.
class SoundBuffer {
public:
    SoundBuffer(std::uint32_t frames, unsigned channels, unsigned bits, std::uint32_t rate);
    void write(std::uint32_t byte_offset, std::span<const std::uint8_t> pcm);
    void play(bool looping) {
        looping_ = looping;
        playing_ = true;
    }
    void stop() { playing_ = false; }
    void seek(std::uint32_t frame);
    void gain(std::uint32_t q15);
    void frequency(std::uint32_t hz);
    bool playing() const { return playing_; }
    std::uint32_t cursor() const;

private:
    friend class AudioMixer;
    void accumulate(std::span<std::int64_t> stereo);
    std::int32_t sample(std::uint32_t frame, unsigned channel) const;
    std::vector<std::uint8_t> pcm_;
    std::uint32_t frames_, rate_, gain_ = 32768;
    unsigned channels_, bits_;
    std::uint64_t position_ = 0;  // source frames times 44100
    bool playing_ = false, looping_ = false;
};
class AudioMixer {
public:
    void add(std::shared_ptr<SoundBuffer> buffer);
    void remove(const std::shared_ptr<SoundBuffer>& buffer);
    void mix(std::span<std::int16_t> stereo);

private:
    std::vector<std::shared_ptr<SoundBuffer>> buffers_;
};
}  // namespace dream::wince
