#include "live.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <thread>

#include "dream/render/background.h"
#include "dream/render/display_list.h"
#include "dream/render/fog.h"
#include "dream/render/framebuffer.h"
#include "dream/render/guard_band.h"
#include "dream/render/input.h"
#include "dream/render/texture.h"
#include "dream/render/vk/offscreen.h"
#include "dream/render/vk/present.h"
#include "dream/render/vk/renderer.h"
#include "dream/render/vk/window.h"
#include "dream/runtime/maple/maple.h"
#include "dream/runtime/wince/dc_hardware.h"

#include <SDL3/SDL_timer.h>

namespace dream::wince {
namespace {
constexpr std::uint32_t kGuestWidth = 640, kGuestHeight = 480;
constexpr std::size_t kVram = mem::DcMemory::kVramSize;
constexpr std::size_t kRegWords = 0x2000 / 4;  // the PowerVR registers, palette memory included
constexpr std::uint32_t reg(const std::uint32_t* regs, std::uint32_t offset) {
    return regs[offset / 4];
}
}  // namespace

struct LiveWindow::Impl {
    explicit Impl(DcHardware& hw) : hardware(hw) {}
    DcHardware& hardware;

    // --- between the emulation and the window thread, under `mutex` --------------------------
    std::mutex mutex;
    std::condition_variable work;
    bool render_pending = false, stopping = false;
    std::vector<std::uint32_t> stream_in;
    std::vector<std::uint8_t> vram_in = std::vector<std::uint8_t>(kVram);
    std::vector<std::uint32_t> regs_in = std::vector<std::uint32_t>(kRegWords);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> stale_in;  // texture ranges rewritten
    enum class Show { None, Render, Pixels } show = Show::None;
    std::vector<std::uint32_t> pixels_in;
    std::uint32_t pixels_w = 0, pixels_h = 0;
    maple::ControllerState pad;
    // Buttons that went down since the last vertical blank, however briefly: the guest reads the
    // pad a few times a second of real time while it runs this slowly, and a tap between two
    // reads would be lost.
    std::uint16_t pressed_since = 0;
    std::atomic<bool> closed{false};
    std::atomic<std::uint64_t> renders{0}, presented{0};

    // --- the emulation's own ----------------------------------------------------------------
    std::vector<std::uint32_t> targets;  // buffers renders were written into
    std::vector<std::uint32_t> decoded;  // a framebuffer read back from video memory
    unsigned reported_presses = 0;
    std::array<std::uint8_t, 16> hold{};  // vertical blanks each pressed button stays down
    bool paced = false;
    std::int64_t pace_offset = 0;  // the wall clock minus the guest's, in nanoseconds
    std::uint64_t waited = 0;      // nanoseconds spent holding the guest back, for reports

    // --- the window thread's own ------------------------------------------------------------
    struct Gfx {
        render::vk::Window window;  // first, so it goes last
        render::vk::Offscreen offscreen;
        render::vk::Renderer renderer;
        render::vk::Presenter presenter;
    };
    std::thread thread;
    std::unique_ptr<Gfx> gfx;
    render::DisplayList decoder;
    render::vk::FrameGeometry geometry;
    std::vector<std::uint32_t> stream;
    std::vector<std::uint8_t> vram = std::vector<std::uint8_t>(kVram);
    std::vector<std::uint32_t> regs = std::vector<std::uint32_t>(kRegWords);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> stale;
    std::vector<std::uint32_t> pixels;
    bool reported = false;

    bool open(const std::string& title, std::string& error);
    void loop();
    void draw();
    void present(const std::uint32_t* px, std::uint32_t w, std::uint32_t h);
    void screenshot(const std::uint32_t* px, std::uint32_t w, std::uint32_t h);
    unsigned shots = 0;
    maple::ControllerState read_pad() const;
};

LiveWindow::LiveWindow(DcHardware& hardware) : impl_(std::make_unique<Impl>(hardware)) {}

LiveWindow::~LiveWindow() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->work.notify_all();
    if (impl_->thread.joinable())
        impl_->thread.join();
}

bool LiveWindow::start(const std::string& title, std::string& error) {
    std::promise<std::string> ready;
    auto opened = ready.get_future();
    impl_->thread = std::thread([this, title, &ready] {
        std::string failure;
        const bool ok = impl_->open(title, failure);
        ready.set_value(ok ? std::string() : (failure.empty() ? "window failed" : failure));
        if (ok)
            impl_->loop();
        impl_->closed = true;
        impl_->gfx.reset();  // the window goes on the thread that made it
    });
    error = opened.get();
    if (error.empty())
        return true;
    impl_->thread.join();
    return false;
}

bool LiveWindow::Impl::open(const std::string& title, std::string& error) {
    gfx = std::make_unique<Gfx>();
    auto& g = *gfx;
    if (!g.window.create(title.c_str(), 960, 720, false)) {
        error = g.window.error();
        return false;
    }
    // Translucency sorted per pixel where the device can, as boot_main does (DREAM_NO_OIT=1 to
    // compare with the per-strip sort).
    g.offscreen.sort_per_pixel = std::getenv("DREAM_NO_OIT") == nullptr;
    if (!g.offscreen.create(g.window.context(), kGuestWidth, kGuestHeight)) {
        error = g.offscreen.error();
        return false;
    }
    if (!g.renderer.create(g.window.context(), g.offscreen.render_pass(),
                           g.offscreen.oit_targets())) {
        error = g.renderer.error();
        return false;
    }
    if (!g.presenter.create(g.window.context(), g.window.render_pass())) {
        error = g.presenter.error();
        return false;
    }
    // One copy of video memory for the whole run: pointing the renderer anywhere else drops
    // every decoded texture.
    g.renderer.set_memory(vram.data(), vram.size(), regs.data() + 0x1000 / 4,
                          render::PaletteFormat::Argb1555);
    // The player's layout, the same file the Katana titles and the launcher read.
    render::Bindings bindings = render::Bindings::defaults();
    const std::string path = render::vk::default_bindings_path();
    if (std::ifstream in(path, std::ios::binary); !path.empty() && in) {
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        std::string warnings;
        bindings = render::Bindings::from_text(text, &warnings);
        if (!warnings.empty())
            std::fprintf(stderr, "%s:\n%s", path.c_str(), warnings.c_str());
    }
    g.window.set_bindings(bindings);
    std::fprintf(stderr, "wince: window on %s\n", g.window.context().caps().device_name.c_str());
    return true;
}

void LiveWindow::Impl::loop() {
    auto& g = *gfx;
    for (;;) {
        if (!g.window.poll())  // closed, or escape
            return;
        const auto held = read_pad();
        bool draw_now = false;
        Show show_now = Show::None;
        std::uint32_t w = 0, h = 0;
        {
            std::unique_lock lock(mutex);
            pad = held;
            pressed_since |= static_cast<std::uint16_t>(~held.buttons);
            // Wake for work, or every few milliseconds to keep the window answering.
            work.wait_for(lock, std::chrono::milliseconds(4),
                          [&] { return stopping || render_pending || show != Show::None; });
            if (stopping)
                return;
            if (render_pending) {
                stream.swap(stream_in);
                std::memcpy(vram.data(), vram_in.data(), kVram);
                std::memcpy(regs.data(), regs_in.data(), kRegWords * 4);
                stale.swap(stale_in);
                stale_in.clear();
                render_pending = false;
                draw_now = true;
            }
            show_now = show;
            show = Show::None;
            if (show_now == Show::Pixels) {
                pixels.swap(pixels_in);
                w = pixels_w;
                h = pixels_h;
            }
        }
        if (draw_now)
            draw();
        if (show_now == Show::Render && g.offscreen.pixels())
            present(g.offscreen.pixels(), g.offscreen.width(), g.offscreen.height());
        else if (show_now == Show::Pixels && w && h && pixels.size() >= std::size_t{w} * h)
            present(pixels.data(), w, h);
    }
}

// What boot_main's Live::render does with a display list, from the copies taken at the render.
void LiveWindow::Impl::draw() {
    auto& g = *gfx;
    decoder.reset();
    decoder.feed_stream(stream.data(), stream.size());
    render::add_background(decoder.frame(), regs.data(), vram.data(), vram.size());
    render::clip_to_guard_band(decoder.frame());
    geometry.alpha_ref = static_cast<float>(reg(regs.data(), 0x11C) & 0xFFu) / 255.0f;
    render::describe_fog(regs.data(), geometry.fog);
    render::vk::describe_write_clip(regs.data(), geometry);
    g.renderer.textures().set_palette(
        regs.data() + 0x1000 / 4, static_cast<render::PaletteFormat>(reg(regs.data(), 0x108) & 3u));
    for (const auto& [lo, hi] : stale) g.renderer.textures().invalidate_range(lo, hi, false);
    stale.clear();
    g.renderer.textures().set_stride_width((reg(regs.data(), 0xE4) & 31u) * 32u);
    if (!g.offscreen.render(g.renderer, decoder.frame(), geometry)) {
        if (!reported) {
            reported = true;
            std::fprintf(stderr, "wince: render: %s\n", g.offscreen.error().c_str());
        }
        return;
    }
    ++renders;
}

void LiveWindow::Impl::present(const std::uint32_t* px, std::uint32_t w, std::uint32_t h) {
    auto& g = *gfx;
    std::uint32_t image = 0;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!g.window.begin_frame(image, cmd)) {
        g.window.recreate_swapchain();
        return;
    }
    g.presenter.upload(cmd, px, w, h);
    VkClearValue clears[2]{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clears[1].depthStencil = {0.0f, 0};
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = g.window.render_pass();
    rpbi.framebuffer = g.window.framebuffer(image);
    rpbi.renderArea.extent = g.window.extent();
    rpbi.clearValueCount = 2;
    rpbi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0.0f,
                              0.0f,
                              static_cast<float>(g.window.extent().width),
                              static_cast<float>(g.window.extent().height),
                              0.0f,
                              1.0f};
    const VkRect2D scissor{{0, 0}, g.window.extent()};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    g.presenter.draw(cmd, g.window.extent());
    vkCmdEndRenderPass(cmd);
    if (!g.window.end_frame(image))
        g.window.recreate_swapchain();
    ++presented;
    // F12, or DREAM_WINCE_SCREENSHOT_AT=N (the Nth image shown): the guest's picture, as shown.
    static const std::vector<std::uint64_t> shot_at = [] {
        std::vector<std::uint64_t> at;  // DREAM_WINCE_SCREENSHOT_AT=N[,N...]
        for (const char* v = std::getenv("DREAM_WINCE_SCREENSHOT_AT"); v && *v;) {
            char* end = nullptr;
            at.push_back(std::strtoull(v, &end, 0));
            v = end && *end == ',' ? end + 1 : nullptr;
        }
        return at;
    }();
    if (g.window.pressed(render::vk::Control::Screenshot) ||
        std::find(shot_at.begin(), shot_at.end(), presented.load()) != shot_at.end())
        screenshot(px, w, h);
}

void LiveWindow::Impl::screenshot(const std::uint32_t* px, std::uint32_t w, std::uint32_t h) {
    char name[48];
    std::snprintf(name, sizeof name, "screenshot-wince-%03u.ppm", ++shots);
    FILE* f = std::fopen(name, "wb");
    if (!f)
        return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (std::size_t i = 0; i < std::size_t{w} * h; ++i) {
        const unsigned char rgb[3] = {static_cast<unsigned char>(px[i]),
                                      static_cast<unsigned char>(px[i] >> 8),
                                      static_cast<unsigned char>(px[i] >> 16)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    std::fprintf(stderr, "wince: wrote %s\n", name);
}

// The Maple controller as the player's bindings read it (boot_main's read_controls): buttons
// active low, the stick and the triggers as values.
maple::ControllerState LiveWindow::Impl::read_pad() const {
    using render::PadControl;
    const auto& w = gfx->window;
    std::uint16_t held = 0;
    const std::pair<PadControl, std::uint16_t> buttons[] = {
        {PadControl::Up, maple::kUp},      {PadControl::Down, maple::kDown},
        {PadControl::Left, maple::kLeft},  {PadControl::Right, maple::kRight},
        {PadControl::A, maple::kA},        {PadControl::B, maple::kB},
        {PadControl::X, maple::kX},        {PadControl::Y, maple::kY},
        {PadControl::Start, maple::kStart}};
    for (const auto& [control, bit] : buttons)
        if (w.pad_held(control))
            held |= bit;
    maple::ControllerState state;
    state.buttons = static_cast<std::uint16_t>(0xFFFFu & ~held);
    state.joy_x =
        maple::axis_byte(w.pad_value(PadControl::StickLeft), w.pad_value(PadControl::StickRight));
    state.joy_y =
        maple::axis_byte(w.pad_value(PadControl::StickUp), w.pad_value(PadControl::StickDown));
    state.ltrigger = maple::trigger_byte(w.pad_value(PadControl::LeftTrigger));
    state.rtrigger = maple::trigger_byte(w.pad_value(PadControl::RightTrigger));
    return state;
}

void LiveWindow::render(const std::vector<std::uint32_t>& stream) {
    auto& d = *impl_;
    auto& pvr = d.hardware.pvr();
    auto& memory = d.hardware.system().memory;
    const std::uint32_t* regs = pvr.reg_block();
    // Where this render goes. A buffer the video output later shows is drawn from the render;
    // a render into a texture is drawn but not written back (not supported here yet).
    const std::uint8_t* vram = memory.vram();
    const render::TileExtent tiles = render::region_extent(regs, [vram](std::uint32_t a) {
        std::uint32_t v = 0;
        std::memcpy(&v, vram + mem::DcMemory::vram_map32(a & (kVram - 4)), 4);
        return v;
    });
    render::FramebufferInfo target;
    if (render::describe_write_framebuffer(regs, kGuestWidth, kGuestHeight, target, &tiles) &&
        !target.texture &&
        std::find(d.targets.begin(), d.targets.end(), target.address) == d.targets.end() &&
        d.targets.size() < 8)
        d.targets.push_back(target.address);
    // Texture memory rewritten since the last render: what the cache decoded from it is stale.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> stale = memory.take_vram_writes();
    std::uint32_t lo = 0, hi = 0;
    if (pvr.take_texture_writes(lo, hi))
        stale.emplace_back(lo, hi);
    {
        std::lock_guard lock(d.mutex);
        d.stream_in = stream;  // a newer render replaces one not drawn yet
        std::memcpy(d.vram_in.data(), vram, kVram);
        std::memcpy(d.regs_in.data(), regs, kRegWords * 4);
        d.stale_in.insert(d.stale_in.end(), stale.begin(), stale.end());
        d.render_pending = true;
    }
    d.work.notify_one();
}

void LiveWindow::vblank() {
    auto& d = *impl_;
    // A window plays in real time. With its busy-waits skipped a title's menu needs a fraction
    // of a frame's time, so the guest is held back to the wall clock here, at 200 MHz; one more
    // than 100 ms behind (a load, a heavy scene) is not made to race to catch up afterwards.
    {
        const auto guest = static_cast<std::int64_t>(d.hardware.system().sched.now() * 5);
        const auto wall = static_cast<std::int64_t>(SDL_GetTicksNS());
        if (!d.paced || wall - d.pace_offset > guest + 100'000'000) {
            d.pace_offset = wall - guest;
            d.paced = true;
        } else if (const auto due = d.pace_offset + guest; due > wall) {
            SDL_DelayPrecise(static_cast<Uint64>(due - wall));
            d.waited += static_cast<std::uint64_t>(due - wall);
        }
    }
    const std::uint32_t* regs = d.hardware.pvr().reg_block();
    Impl::Show show = Impl::Show::None;
    render::FramebufferInfo fb;
    if (render::describe_framebuffer(regs, fb) && fb.enabled) {
        // The buffer on screen is one the renderer drew into: show the drawing. Otherwise it
        // holds what the title wrote itself (DirectDraw blits, a video), read from video memory.
        if (std::find(d.targets.begin(), d.targets.end(), fb.address) != d.targets.end())
            show = Impl::Show::Render;
        else if (render::decode_framebuffer(fb, d.hardware.system().memory.vram(), kVram,
                                            d.decoded))
            show = Impl::Show::Pixels;
    }
    maple::ControllerState held;
    {
        std::lock_guard lock(d.mutex);
        if (show == Impl::Show::Pixels) {
            d.pixels_in.swap(d.decoded);
            d.pixels_w = fb.width;
            d.pixels_h = fb.height;
        }
        if (show != Impl::Show::None)
            d.show = show;
        held = d.pad;
        // A press, however short, is seen down for at least three vertical blanks: the title
        // looks for a button going down between two of its reads.
        const std::uint16_t down = d.pressed_since | static_cast<std::uint16_t>(~held.buttons);
        d.pressed_since = 0;
        std::uint16_t shown_down = 0;
        for (unsigned b = 0; b < 16; ++b) {
            if (down & (1u << b))
                d.hold[b] = 3;
            if (d.hold[b]) {
                --d.hold[b];
                shown_down |= static_cast<std::uint16_t>(1u << b);
            }
        }
        held.buttons = static_cast<std::uint16_t>(~shown_down);
    }
    if (show != Impl::Show::None)
        d.work.notify_one();
    // The first changes of the buttons go to the log: whether the player's presses reach the
    // console is the first thing to know when a title seems not to answer.
    if (held.buttons != d.hardware.pad().state.buttons && d.reported_presses < 40) {
        ++d.reported_presses;
        std::fprintf(stderr, "wince: pad buttons %04x at frame %llu\n", held.buttons ^ 0xFFFFu,
                     static_cast<unsigned long long>(d.hardware.system().spg.frames()));
    }
    d.hardware.pad().state = held;
}

bool LiveWindow::closed() const {
    return impl_->closed;
}
std::uint64_t LiveWindow::renders() const {
    return impl_->renders;
}
std::uint64_t LiveWindow::presented() const {
    return impl_->presented;
}
double LiveWindow::waited_seconds() const {
    return static_cast<double>(impl_->waited) / 1e9;
}
}  // namespace dream::wince
