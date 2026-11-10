// A window on the emulated console for dream-wince (built with the renderer only): what the
// PowerVR draws and the video output scans out, and the player's pad on the Maple bus.
//
// The title's threads each run on a host thread of their own, and a window answers the one thread
// that created it, so the window, Vulkan and all drawing live on a thread of their own here. The
// emulation hands it work: at each render the display list with a copy of video memory and the
// PowerVR registers (the guest goes on changing both while the frame is drawn), at each vertical
// blank what to show. The window thread hands back the pad, read through the player's bindings
// (the file the Katana titles and the launcher share).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dream::wince {
class DcHardware;

class LiveWindow {
public:
    explicit LiveWindow(DcHardware& hardware);
    ~LiveWindow();
    LiveWindow(const LiveWindow&) = delete;
    LiveWindow& operator=(const LiveWindow&) = delete;

    // Opens the window (on its own thread) and waits until it is up. False with `error` set.
    bool start(const std::string& title, std::string& error);
    // Emulation side, with the baton held. The guest started a render of `stream`.
    void render(const std::vector<std::uint32_t>& stream);
    // The vertical blank: what the video output shows goes to the window, and the pad as the
    // player holds it now goes onto the Maple bus. Waits first while the guest's clock is ahead
    // of the wall's: a window plays in real time.
    void vblank();
    // The player closed the window (or pressed escape): the run should end.
    bool closed() const;

    std::uint64_t renders() const;
    std::uint64_t presented() const;
    // Time vblank() spent holding the guest back to the wall clock: the room left.
    double waited_seconds() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace dream::wince
