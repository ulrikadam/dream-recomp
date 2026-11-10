// A title's CD audio, heard (built with SDL3 only). On the console the drive plays its audio
// tracks by itself once told to; here a thread of its own streams the disc image's audio sectors
// to the sound device in real time, whatever pace the guest keeps, so the music plays at its
// speed even while the title runs slowly.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "dream/runtime/gdrom/disc.h"
#include "dream/runtime/wince/system_services.h"

namespace dream::wince {

class CdAudio final : public CdAudioOutput {
public:
    explicit CdAudio(std::unique_ptr<gdrom::Disc> disc);
    ~CdAudio() override;
    CdAudio(const CdAudio&) = delete;
    CdAudio& operator=(const CdAudio&) = delete;

    // Opens the sound device and starts the thread. False with `error` set (no device).
    bool start(std::string& error);

    void play(std::uint32_t start_fad, std::uint32_t end_fad) override;
    void stop() override;
    void pause() override;
    void resume() override;
    void volume(std::uint8_t left, std::uint8_t right) override;

    std::uint64_t sectors_played() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dream::wince
