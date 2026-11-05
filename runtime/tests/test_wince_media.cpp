#include <array>
#include <limits>
#include <stdexcept>

#include "dream/runtime/wince/media.h"

#include "doctest.h"

using namespace dream::wince;

TEST_CASE("WinCE graphics: RGB565 pitch lock and conversion to presentation pixels") {
    Surface s(3, 2, PixelFormat::Rgb565);
    CHECK(s.pitch() == 8);
    s.fill({0, 0, 3, 2}, 0xFF0000);
    CHECK(s.pixel(2, 1) == 0xFFFF0000);
    auto bytes = s.lock();
    CHECK(bytes.size() == 16);
    CHECK(bytes[0] == 0);
    CHECK(bytes[1] == 0xF8);
    bytes[0] = 0xE0;
    bytes[1] = 7;  // green
    CHECK_THROWS_AS(s.lock(), std::runtime_error);
    CHECK_THROWS_AS(s.fill({0, 0, 1, 1}, 0), std::runtime_error);
    CHECK_THROWS_AS(s.rgba(), std::runtime_error);
    s.unlock();
    CHECK(s.pixel(0, 0) == 0xFF00FF00);
    CHECK(s.rgba().size() == 6);
    CHECK_THROWS_AS(s.unlock(), std::runtime_error);
    CHECK_THROWS_AS(s.pixel(3, 0), std::out_of_range);
    CHECK_THROWS_AS(Surface(0xFFFFFFFF, 1, PixelFormat::Rgb565), std::invalid_argument);
}

TEST_CASE("WinCE graphics: blits color keys overlap and boundary validation") {
    Surface s(4, 1, PixelFormat::Xrgb8888);
    for (unsigned x = 0; x < 4; ++x) s.pixel(x, 0, x + 1);
    s.blit(1, 0, s, {0, 0, 3, 1});
    CHECK(s.pixel(1, 0) == 0xFF000001);
    CHECK(s.pixel(3, 0) == 0xFF000003);
    Surface d(4, 1, PixelFormat::Xrgb8888);
    d.fill({0, 0, 4, 1}, 0xFFFFFF);
    d.blit(0, 0, s, {0, 0, 4, 1}, 1);
    CHECK(d.pixel(1, 0) == 0xFFFFFFFF);
    CHECK(d.pixel(2, 0) == 0xFF000002);
    CHECK_THROWS_AS(d.blit(3, 0, s, {0, 0, 4, 1}), std::out_of_range);
    CHECK(d.pixel(3, 0) == 0xFF000003);
}

TEST_CASE("WinCE graphics: triangle coverage Gouraud depth and winding") {
    Surface s(8, 8, PixelFormat::Xrgb8888);
    Rasterizer r(s);
    const TlVertex a{0, 0, 0.5f, 0xFF0000}, b{8, 0, 0.5f, 0x00FF00}, c{0, 8, 0.5f, 0x0000FF};
    r.triangle(a, b, c);
    const auto sample = s.pixel(1, 1);
    CHECK((sample & 0xFF0000) != 0);
    CHECK((sample & 0x00FF00) != 0);
    CHECK((sample & 255) != 0);
    CHECK(s.pixel(7, 7) == 0xFF000000);
    r.triangle({0, 0, 0.8f, 0xFFFFFF}, {8, 0, 0.8f, 0xFFFFFF}, {0, 8, 0.8f, 0xFFFFFF});
    CHECK(s.pixel(1, 1) == sample);
    r.triangle({0, 0, 0.2f, 0xFFFFFF}, {0, 8, 0.2f, 0xFFFFFF}, {8, 0, 0.2f, 0xFFFFFF});
    CHECK(s.pixel(1, 1) == 0xFFFFFFFF);
    CHECK_THROWS_AS(r.triangle({std::numeric_limits<float>::quiet_NaN(), 0, 0, 0}, b, c),
                    std::invalid_argument);
    r.clear_depth();
    r.triangle(a, b, c);
    CHECK(s.pixel(1, 1) == sample);
}

TEST_CASE("WinCE graphics: two triangles tile a rectangle without cracks") {
    Surface s(4, 4, PixelFormat::Xrgb8888);
    Rasterizer r(s);
    r.triangle({0, 0, 0, 0xFF0000}, {4, 0, 0, 0xFF0000}, {0, 4, 0, 0xFF0000});
    r.triangle({4, 0, 0, 0x00FF00}, {4, 4, 0, 0x00FF00}, {0, 4, 0, 0x00FF00});
    for (auto pixel : s.rgba()) CHECK(pixel != 0xFF000000);
}

TEST_CASE("WinCE audio: PCM16 mono duplication looping stop and cursor") {
    auto b = std::make_shared<SoundBuffer>(2, 1, 16, 44100);
    const std::array<std::uint8_t, 4> pcm = {0xE8, 3, 0x18, 0xFC};  // +1000, -1000
    b->write(0, pcm);
    AudioMixer mixer;
    mixer.add(b);
    std::array<std::int16_t, 6> out{};
    b->play(false);
    mixer.mix(out);
    CHECK(out == std::array<std::int16_t, 6>{1000, 1000, -1000, -1000, 0, 0});
    CHECK_FALSE(b->playing());
    CHECK(b->cursor() == 0);
    b->play(true);
    mixer.mix(out);
    CHECK(out == std::array<std::int16_t, 6>{1000, 1000, -1000, -1000, 1000, 1000});
    CHECK(b->cursor() == 1);
    CHECK(b->playing());
    b->stop();
    mixer.mix(out);
    CHECK(out == std::array<std::int16_t, 6>{});
    CHECK(b->cursor() == 1);
    b->seek(0);
    CHECK(b->cursor() == 0);
    CHECK_THROWS_AS(b->seek(2), std::out_of_range);
}

TEST_CASE("WinCE audio: PCM8 stereo resampling wrapping and final mix saturation") {
    auto b = std::make_shared<SoundBuffer>(2, 2, 8, 22050);
    const std::array<std::uint8_t, 4> pcm = {255, 0, 128, 128};
    b->write(2, pcm);  // wraps: silent frame then +32512,-32768
    b->play(true);
    AudioMixer mixer;
    mixer.add(b);
    mixer.add(b);  // one voice, even if added twice
    std::array<std::int16_t, 8> out{};
    mixer.mix(out);
    CHECK(out == std::array<std::int16_t, 8>{0, 0, 0, 0, 32512, -32768, 32512, -32768});
    auto loud = std::make_shared<SoundBuffer>(1, 2, 8, 44100);
    const std::array<std::uint8_t, 2> loud_pcm = {255, 0};
    loud->write(0, loud_pcm);
    loud->play(true);
    mixer.add(loud);
    b->seek(1);
    mixer.mix(out);
    CHECK(out[0] == 32767);
    CHECK(out[1] == -32768);
    mixer.remove(loud);
    b->gain(16384);
    b->seek(1);
    mixer.mix(out);
    CHECK(out[0] == 16256);
    CHECK(out[1] == -16384);
    CHECK_THROWS_AS(SoundBuffer(0, 1, 16, 44100), std::invalid_argument);
    CHECK_THROWS_AS(SoundBuffer(1, 3, 16, 44100), std::invalid_argument);
    CHECK_THROWS_AS(mixer.mix(std::span<std::int16_t>(out.data(), 3)), std::invalid_argument);
}
