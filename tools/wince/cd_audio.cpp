#include "cd_audio.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "dream/audio/sink.h"

namespace dream::wince {

struct CdAudio::Impl {
    explicit Impl(std::unique_ptr<gdrom::Disc> d) : disc(std::move(d)) {}
    std::unique_ptr<gdrom::Disc> disc;
    audio::Sink sink;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    bool quit = false, playing = false, paused = false;
    std::uint32_t fad = 0, end = 0;   // the next sector to play, and where to stop
    float left = 1.0f, right = 1.0f;  // the title's volume, 0 to 1
    std::atomic<std::uint64_t> sectors{0};

    void loop();
};

CdAudio::CdAudio(std::unique_ptr<gdrom::Disc> disc)
    : impl_(std::make_unique<Impl>(std::move(disc))) {}

CdAudio::~CdAudio() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->quit = true;
    }
    impl_->wake.notify_all();
    if (impl_->thread.joinable())
        impl_->thread.join();
}

bool CdAudio::start(std::string& error) {
    if (!impl_->sink.open()) {
        error = impl_->sink.error();
        return false;
    }
    impl_->thread = std::thread([this] { impl_->loop(); });
    return true;
}

// One audio sector is 588 stereo frames of 16-bit little-endian PCM at 44.1 kHz (a GDI's .raw
// tracks store it as is). About 190 ms are kept queued: enough to ride out a busy host, little
// enough that a stop is heard at once.
void CdAudio::Impl::loop() {
    std::vector<std::uint8_t> raw(2352);
    for (;;) {
        std::uint32_t at = 0;
        float l = 1.0f, r = 1.0f;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return quit || (playing && !paused); });
            if (quit)
                return;
            if (fad >= end) {
                playing = false;
                continue;
            }
            at = fad++;
            l = left;
            r = right;
        }
        const auto* track = disc->track_for(at - gdrom::kFadOffset);
        if (!track || track->data || track->sector_size != 2352 ||
            !disc->read_raw(at - gdrom::kFadOffset, raw.data())) {
            std::lock_guard lock(mutex);
            playing = false;  // off the audio: the drive stops
            continue;
        }
        for (std::size_t i = 0; i + 4 <= raw.size(); i += 4) {
            const auto sample = [&](std::size_t o) {
                return static_cast<std::int16_t>(raw[o] | raw[o + 1] << 8);
            };
            sink.push(static_cast<std::int16_t>(sample(i) * l),
                      static_cast<std::int16_t>(sample(i + 2) * r));
        }
        ++sectors;
        sink.wait_below(8192, 100);
    }
}

void CdAudio::play(std::uint32_t start_fad, std::uint32_t end_fad) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->fad = start_fad;
        impl_->end = end_fad;
        impl_->playing = true;
        impl_->paused = false;
    }
    impl_->wake.notify_all();
}
void CdAudio::stop() {
    std::lock_guard lock(impl_->mutex);
    impl_->playing = false;
}
void CdAudio::pause() {
    std::lock_guard lock(impl_->mutex);
    impl_->paused = true;
}
void CdAudio::resume() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->paused = false;
    }
    impl_->wake.notify_all();
}
void CdAudio::volume(std::uint8_t left, std::uint8_t right) {
    std::lock_guard lock(impl_->mutex);
    impl_->left = static_cast<float>(left) / 255.0f;
    impl_->right = static_cast<float>(right) / 255.0f;
}
std::uint64_t CdAudio::sectors_played() const {
    return impl_->sectors;
}

}  // namespace dream::wince
