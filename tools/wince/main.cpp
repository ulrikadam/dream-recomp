#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string_view>
#include <thread>

#include "dream/runtime/fenv.h"
#include "dream/runtime/gdrom/disc.h"
#include "dream/runtime/wince/dc_hardware.h"
#include "dream/runtime/wince/directx.h"
#include "dream/runtime/wince/media.h"
#include "dream/runtime/wince/modules.h"
#include "dream/runtime/wince/process.h"
#include "dream/runtime/wince/system_services.h"
#include "dream/runtime/wince/threads.h"
#include "dream/runtime/wince/wdm_host.h"
#ifdef DREAM_WINCE_WINDOW
#include "live.h"
#endif
#ifdef DREAM_WINCE_AUDIO
#include "dream/audio/sink.h"

#include "cd_audio.h"
#endif
#ifdef DREAM_DEV_INTERPRETER
#include "dream/runtime/devinterp/interpreter.h"
#endif

namespace {
using namespace dream::wince;
// The --steps budget (all threads, all runs) is spent.
struct StepBudget {};
void help() {
    std::cout
        << "Experimental Windows CE / SH-4 development tools (not a game launcher)\n"
           "dream-wince --check module.exe [--base 0x10000]\n"
           "dream-wince --run module.exe [--base 0x10000] [--steps 100000] [--capture directory]\n"
           "            [--real-directx]  run the disc's DDRAW/DSOUND, not replacements\n"
           "            [--lazy]  link unsupported COREDLL/DDRAW/DSOUND imports to stubs that\n"
           "                      stop the run, naming the function, only if it is called\n"
           "            [--window]  show the console live and read the pad (builds with the\n"
           "                        renderer); runs until the window is closed, with sound\n"
           "            [--no-audio]  a silent window\n"
           "            [--no-wait-skip]  run busy-waits on the graphics (DDERR_WASSTILLDRAWING\n"
           "                              polls) instead of moving on to the next interrupt\n"
           "            [--vmu card.bin]  the memory card in the pad (saves written back; made\n"
           "                              blank if missing)\n"
           "            [--dump-ta F] [--dump-ta-at-frame N] [--dump-ta-last F]  save a render\n"
           "            [--disc image.gdi]  the disc, for the CD-ROM device (tracks, CD audio)\n"
           "dream-wince --inspect module.exe\n"
           "dream-wince --audit module.exe\n"
           "dream-wince --media-demo output-directory\n"
           "Dependencies are read beside the executable. No host DLL is executed.\n"
           "Supports the documented COREDLL and guest DirectX COM subsets only.\n";
}
std::uint32_t number(std::string_view text) {
    int base = 10;
    if (text.starts_with("0x")) {
        base = 16;
        text.remove_prefix(2);
    }
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::invalid_argument("invalid integer argument");
    return value;
}
std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("cannot open PE file");
    const auto end = f.tellg();
    if (end < 0 || end > 32 * 1024 * 1024)
        throw std::runtime_error("PE file exceeds 32 MiB limit");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f)
        throw std::runtime_error("cannot read PE file");
    return bytes;
}
void inspect(const Image& image) {
    std::cout << "PE32 SH-4: base=0x" << std::hex << image.base << " entry=0x" << image.entry
              << std::dec << " bytes=" << image.bytes.size() << " imports=" << image.imports.size()
              << " exports=" << image.exports.size() << " dll=" << image.dll << '\n';
    for (const auto& s : image.imports)
        std::cout << "import " << s.dll << '!'
                  << (s.ordinal ? "#" + std::to_string(*s.ordinal) : s.name) << '\n';
    for (const auto& s : image.exports)
        std::cout << "export #" << s.ordinal << ' ' << s.name << " rva=0x" << std::hex << s.rva
                  << std::dec << (s.forwarder.empty() ? "" : " -> " + s.forwarder) << '\n';
}
void le16(std::ostream& out, std::uint16_t n) {
    out.put(static_cast<char>(n & 255));
    out.put(static_cast<char>(n >> 8));
}
void le32(std::ostream& out, std::uint32_t n) {
    le16(out, static_cast<std::uint16_t>(n));
    le16(out, static_cast<std::uint16_t>(n >> 16));
}
#ifdef DREAM_DEV_INTERPRETER
void capture(DirectX& dx, const std::filesystem::path& dir) {
    const auto picture = dir / "guest.ppm", sound = dir / "guest.wav";
    if (std::filesystem::exists(picture) || std::filesystem::exists(sound))
        throw std::runtime_error("capture outputs already exist; select a new directory");
    std::filesystem::create_directories(dir);
    if (!dx.frame().empty()) {
        std::ofstream out(picture, std::ios::binary);
        out << "P6\n" << dx.width() << ' ' << dx.height() << "\n255\n";
        for (auto p : dx.frame()) {
            out.put(static_cast<char>(p >> 16));
            out.put(static_cast<char>(p >> 8));
            out.put(static_cast<char>(p));
        }
        out.close();
        if (!out)
            throw std::runtime_error("cannot write captured frame");
    }
    // Offline capture: advance the deterministic mixer by one second after the guest returns.
    std::vector<std::int16_t> pcm(88200);
    dx.mix(pcm);
    std::ofstream out(sound, std::ios::binary);
    out.write("RIFF", 4);
    le32(out, 36 + 176400);
    out.write("WAVEfmt ", 8);
    le32(out, 16);
    le16(out, 1);
    le16(out, 2);
    le32(out, 44100);
    le32(out, 176400);
    le16(out, 4);
    le16(out, 16);
    out.write("data", 4);
    le32(out, 176400);
    for (auto s : pcm) le16(out, static_cast<std::uint16_t>(s));
    out.close();
    if (!out)
        throw std::runtime_error("cannot write captured audio");
    std::cout << "Guest capture: presents=" << dx.presents() << " output=" << dir.string() << '\n';
}
#endif
void media_demo(const std::filesystem::path& dir) {
    const auto picture = dir / "wince-backend.ppm", sound = dir / "wince-backend.wav";
    if (std::filesystem::exists(picture) || std::filesystem::exists(sound))
        throw std::runtime_error("demo outputs already exist; select a new directory");
    std::filesystem::create_directories(dir);
    Surface target(320, 240, PixelFormat::Xrgb8888);
    target.fill({0, 0, 320, 240}, 0x142033);
    Surface banner(80, 16, PixelFormat::Rgb565);
    banner.fill({0, 0, 80, 16}, 0x20B0E0);
    target.blit(16, 16, banner, {0, 0, 80, 16});
    Rasterizer raster(target);
    raster.triangle({160, 45, 0.5f, 0xFF6040}, {285, 215, 0.5f, 0x40FF80},
                    {35, 215, 0.5f, 0x4080FF});
    std::ofstream image(picture, std::ios::binary);
    image << "P6\n320 240\n255\n";
    for (auto pixel : target.rgba()) {
        image.put(static_cast<char>((pixel >> 16) & 255));
        image.put(static_cast<char>((pixel >> 8) & 255));
        image.put(static_cast<char>(pixel & 255));
    }
    image.close();
    if (!image)
        throw std::runtime_error("cannot write demo picture");
    auto buffer = std::make_shared<SoundBuffer>(44100, 1, 16, 44100);
    std::vector<std::uint8_t> pcm(88200);
    for (unsigned i = 0; i < 44100; ++i) {
        // Short fade at each end; generated tone, no external media or game data.
        const auto envelope = std::min(1.0, std::min(i / 441.0, (44099u - i) / 441.0));
        const auto sample = static_cast<std::int16_t>(
            std::sin(6.283185307179586 * 440 * i / 44100) * 6000 * envelope);
        const auto raw = static_cast<std::uint16_t>(sample);
        pcm[i * 2] = static_cast<std::uint8_t>(raw);
        pcm[i * 2 + 1] = static_cast<std::uint8_t>(raw >> 8);
    }
    buffer->write(0, pcm);
    buffer->play(false);
    AudioMixer mixer;
    mixer.add(buffer);
    std::vector<std::int16_t> samples(88200);
    mixer.mix(samples);
    std::ofstream audio(sound, std::ios::binary);
    audio.write("RIFF", 4);
    le32(audio, 36 + 176400);
    audio.write("WAVEfmt ", 8);
    le32(audio, 16);
    le16(audio, 1);
    le16(audio, 2);
    le32(audio, 44100);
    le32(audio, 176400);
    le16(audio, 4);
    le16(audio, 16);
    audio.write("data", 4);
    le32(audio, 176400);
    for (auto sample : samples) le16(audio, static_cast<std::uint16_t>(sample));
    audio.close();
    if (!audio)
        throw std::runtime_error("cannot write demo sound");
    std::cout << "CPU media backend demo written to " << dir.string() << "\n";
}
}  // namespace

int main(int argc, char** argv) {
    dream::sh4::Ctx cpu{};
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            help();
            return 0;
        }
        if (argc < 3) {
            help();
            return 2;
        }
        const std::string_view mode = argv[1];
        if (mode == "--media-demo") {
            if (argc != 3)
                throw std::invalid_argument("unexpected media demo arguments");
            media_demo(argv[2]);
            return 0;
        }
        if (mode != "--check" && mode != "--run" && mode != "--inspect" && mode != "--audit")
            throw std::invalid_argument("unknown command");
        std::optional<std::uint32_t> base;
        std::uint64_t steps = 100000;
        bool steps_given = false;
        std::optional<std::filesystem::path> capture_dir;
        std::string dump_ta, dump_last, vmu, disc_image;
        std::uint64_t dump_ta_at_frame = 0;
        bool lazy = false, register_servers = false, real_directx = false, window = false,
             no_audio = false, no_wait_skip = false;
        for (int i = 3; i < argc; i += 2) {
            if (std::string_view(argv[i]) == "--window" && mode == "--run") {
                window = true;
                --i;
                continue;
            }
            if (std::string_view(argv[i]) == "--no-audio" && mode == "--run") {
                no_audio = true;
                --i;
                continue;
            }
            if (std::string_view(argv[i]) == "--no-wait-skip" && mode == "--run") {
                no_wait_skip = true;
                --i;
                continue;
            }
            if (std::string_view(argv[i]) == "--lazy" && (mode == "--run" || mode == "--check")) {
                lazy = true;
                --i;  // a flag, no value
                continue;
            }
            if (std::string_view(argv[i]) == "--register" && mode == "--run") {
                register_servers = true;
                --i;
                continue;
            }
            if (std::string_view(argv[i]) == "--real-directx") {
                real_directx = true;
                --i;
                continue;
            }
            if (i + 1 == argc)
                throw std::invalid_argument("missing option value");
            const std::string_view option = argv[i];
            if (option == "--base")
                base = number(argv[i + 1]);
            else if (option == "--steps") {
                steps = number(argv[i + 1]);
                steps_given = true;
            } else if (option == "--capture" && mode == "--run")
                capture_dir = argv[i + 1];
            else if (option == "--dump-ta" && mode == "--run")
                dump_ta = argv[i + 1];
            else if (option == "--dump-ta-at-frame" && mode == "--run")
                dump_ta_at_frame = number(argv[i + 1]);
            else if (option == "--dump-ta-last" && mode == "--run")
                dump_last = argv[i + 1];
            else if (option == "--vmu" && mode == "--run")
                vmu = argv[i + 1];
            else if (option == "--disc" && mode == "--run")
                disc_image = argv[i + 1];
            else
                throw std::invalid_argument("unknown option");
        }
        if (!steps)
            throw std::invalid_argument("step limit must be positive");
        // --window: played rather than measured, so it runs until the window is closed unless a
        // limit is asked for.
        if (window && !steps_given)
            steps = ~std::uint64_t{0};
#ifndef DREAM_WINCE_WINDOW
        if (window)
            throw std::invalid_argument("--window needs a build with the renderer");
#endif
        const auto file = read_file(argv[2]);
        if (mode == "--inspect") {
            inspect(prepare_pe(file, base));
            return 0;
        }
        Kernel kernel;
        ProcessMemory memory;
        // The Dreamcast's devices (PowerVR, video timing, Holly) behind kernel-mode accesses.
        // Building them sets the host rounding mode to Katana's: give the guest's back.
        std::function<std::uint64_t()> guest_clock = [&] { return cpu.cycles; };
        DcHardware hardware([&] { return guest_clock(); });
        dream::fenv::apply(dream::fenv::from_fpscr(cpu.fpscr));
        memory.set_hardware(&hardware);
#ifdef DREAM_WINCE_WINDOW
        // --window: the console live in a window of its own, and the pad read from the player.
        std::unique_ptr<LiveWindow> live;
        if (window) {
            live = std::make_unique<LiveWindow>(hardware);
            std::string error;
            const auto title = "dream-wince: " + std::filesystem::path(argv[2]).filename().string();
            if (!live->start(title, error))
                throw std::runtime_error("window: " + error);
        }
#endif
        // Sound follows the window: a run with a picture is played, a run without is measured.
        // The AICA's output (the title's effects, mixed by its DSOUND on the ARM7) goes out as it
        // is made, on the guest's clock; the CD audio (--disc, below) streams on the host's.
        const bool audio = window && !no_audio;
#ifdef DREAM_WINCE_AUDIO
        dream::audio::Sink effects;
        std::unique_ptr<CdAudio> cd_audio;
        if (audio) {
            if (effects.open())
                hardware.aica().on_sample = [&](std::int16_t l, std::int16_t r) {
                    effects.push(l, r);
                };
            else
                std::fprintf(stderr, "wince: no sound: %s\n", effects.error().c_str());
        }
#else
        if (audio)
            std::fprintf(stderr, "wince: no sound in this build (SDL3 was not found)\n");
#endif
        // DREAM_WINCE_PRESS=start@300,a@420: scripted pad presses, each held 30 frames from that
        // vertical blank on, on top of the player's (a bring-up aid: does the title answer?).
        struct Press {
            std::uint16_t bit;
            std::uint64_t frame;
        };
        std::vector<Press> presses;
        if (const char* v = std::getenv("DREAM_WINCE_PRESS"))
            for (std::string_view rest(v); !rest.empty();) {
                const auto comma = rest.find(',');
                const auto item = rest.substr(0, comma);
                rest = comma == rest.npos ? std::string_view() : rest.substr(comma + 1);
                const auto at = item.find('@');
                if (at == item.npos)
                    continue;
                static const std::pair<std::string_view, std::uint16_t> kButtons[] = {
                    {"start", dream::maple::kStart}, {"a", dream::maple::kA},
                    {"b", dream::maple::kB},         {"x", dream::maple::kX},
                    {"y", dream::maple::kY},         {"up", dream::maple::kUp},
                    {"down", dream::maple::kDown},   {"left", dream::maple::kLeft},
                    {"right", dream::maple::kRight}};
                for (const auto& [name, bit] : kButtons)
                    if (item.substr(0, at) == name)
                        presses.push_back({bit, number(item.substr(at + 1))});
            }
        // DREAM_WINCE_FRAMES=N: the renders started in each N vertical blanks, to see how many
        // frames a scene draws per second of guest time.
        const unsigned frames_every = [] {
            const char* v = std::getenv("DREAM_WINCE_FRAMES");
            return v ? static_cast<unsigned>(std::atoi(v)) : 0u;
        }();
        std::uint64_t renders_before = 0;
#ifdef DREAM_WINCE_WINDOW
        double waited_before = 0;
#endif
        auto frames_wall = std::chrono::steady_clock::now();
        hardware.on_vblank = [&] {
            if (frames_every) {
                const auto frame = hardware.system().spg.frames();
                if (frame && frame % frames_every == 0) {
                    const auto now = std::chrono::steady_clock::now();
                    // In a window, also the time spent waiting for the wall clock: the room the
                    // emulation has left at full speed.
                    double waited = -1;
#ifdef DREAM_WINCE_WINDOW
                    if (live) {
                        waited = live->waited_seconds() - waited_before;
                        waited_before = live->waited_seconds();
                    }
#endif
                    std::fprintf(
                        stderr, "frames %llu: %llu renders in %.1f s",
                        static_cast<unsigned long long>(frame),
                        static_cast<unsigned long long>(hardware.pvr().renders - renders_before),
                        std::chrono::duration<double>(now - frames_wall).count());
                    if (waited >= 0)
                        std::fprintf(stderr, " (%.1f s waiting)", waited);
                    std::fputc('\n', stderr);
                    renders_before = hardware.pvr().renders;
                    frames_wall = now;
                }
            }
            bool shown = false;
#ifdef DREAM_WINCE_WINDOW
            if (live) {
                live->vblank();
                shown = true;
            }
#endif
            if (presses.empty())
                return;
            auto& state = hardware.pad().state;
            if (!shown)
                state.buttons = 0xFFFF;
            const auto frame = hardware.system().spg.frames();
            for (const auto& p : presses)
                if (frame >= p.frame && frame < p.frame + 30)
                    state.buttons = static_cast<std::uint16_t>(state.buttons & ~p.bit);
        };
        // DREAM_WINCE_TRACE_MAPLE=N: the first N Maple frames other than pad reads, with replies.
        if (const char* n = std::getenv("DREAM_WINCE_TRACE_MAPLE")) {
            hardware.maple().on_frame =
                [left = std::atoi(n), &hardware](
                    unsigned port, std::uint8_t recipient, std::uint8_t command,
                    const std::vector<std::uint32_t>& args, std::uint8_t reply) mutable {
                    // Pad reads and the scan of empty ports come every frame: left out.
                    if (command == dream::maple::kGetCondition || reply == 0xFF || left <= 0)
                        return;
                    --left;
                    std::fprintf(
                        stderr, "maple: @%llu port %u to 0x%02x command %u",
                        static_cast<unsigned long long>(hardware.system().sched.now() / 200000),
                        port, recipient, command);
                    for (std::size_t i = 0; i < args.size() && i < 3; ++i)
                        std::fprintf(stderr, " %08x", args[i]);
                    std::fprintf(stderr, "%s -> reply %u\n", args.size() > 3 ? " ..." : "", reply);
                };
        }
        // --vmu: the memory card in the pad's first slot is that image (an existing card only:
        // saves are written back to it). Without it the card is blank and lives in memory.
        if (!vmu.empty()) {
            const auto status = hardware.load_card(vmu);
            if (status == dream::maple::CardStatus::Missing) {
                // A card that does not exist yet is made: blank, formatted, saved there.
                hardware.card().format();
                if (!hardware.card().save_as(vmu))
                    throw std::invalid_argument("--vmu: cannot create a memory card at " + vmu);
                std::fprintf(stderr, "wince: memory card %s created, formatted and empty\n",
                             vmu.c_str());
            } else if (status != dream::maple::CardStatus::Ok) {
                throw std::invalid_argument("--vmu: not a 128 KB memory card image: " + vmu);
            }
        }
        std::uint64_t renders = 0;
        bool dumped = false;
        // A render as dream_render_view reads it: the display list, then video memory and the
        // PowerVR registers beside it.
        const auto save_render = [&](const std::string& name,
                                     const std::vector<std::uint32_t>& stream) {
            auto save = [](const std::string& file, const void* data, std::size_t bytes) {
                if (FILE* f = std::fopen(file.c_str(), "wb")) {
                    std::fwrite(data, 1, bytes, f);
                    std::fclose(f);
                }
            };
            save(name, stream.data(), stream.size() * 4);
            save(name + ".vram", hardware.system().memory.vram(), dream::mem::DcMemory::kVramSize);
            save(name + ".vram.regs", hardware.pvr().reg_block(), 0x2000);
        };
        // --dump-ta-last: the last render, written when the run ends however it ends.
        std::vector<std::uint32_t> last_stream;
        std::uint64_t last_frame = 0;
        hardware.pvr().on_render = [&](const std::vector<std::uint32_t>& stream) {
            ++renders;
#ifdef DREAM_WINCE_WINDOW
            if (live)
                live->render(stream);
#endif
            const auto frame = hardware.system().spg.frames();
            if (!dump_last.empty() && !stream.empty()) {
                last_stream = stream;
                last_frame = frame;
            }
            if (dump_ta.empty() || dumped || frame < dump_ta_at_frame)
                return;
            dumped = true;
            save_render(dump_ta, stream);
            std::printf("wrote %s: render %llu at frame %llu, %zu parameter words\n",
                        dump_ta.c_str(), static_cast<unsigned long long>(renders),
                        static_cast<unsigned long long>(frame), stream.size());
        };
        const auto save_last = [&] {
            if (dump_last.empty() || last_stream.empty())
                return;
            save_render(dump_last, last_stream);
            std::printf("wrote %s: the last render, at frame %llu\n", dump_last.c_str(),
                        static_cast<unsigned long long>(last_frame));
        };
        DirectX dx(memory);
        const auto path = std::filesystem::absolute(argv[2]);
        const auto directory = std::filesystem::canonical(path.parent_path());
        kernel.set_file_root(directory);
        // The OS image's start-up (SYSSTART.EXE) loads the sound driver before any title runs.
        for (const auto& item : std::filesystem::directory_iterator(directory)) {
            auto name = item.path().filename().string();
            for (auto& ch : name)
                if (ch >= 'A' && ch <= 'Z')
                    ch = static_cast<char>(ch + 32);
            if (name == "armcom.drv" && item.is_regular_file()) {
                const auto driver = read_file(item.path());
                hardware.boot_sound(driver);
                std::fprintf(stderr, "wince: sound driver armcom.drv (%zu bytes) started\n",
                             driver.size());
            }
        }
        // Kernel-mode DLLs (PLATUTIL.DLL) call the Dreamcast BIOS through its vectors.
        for (unsigned i = 0; i < SystemServices::kBiosVectors.size(); ++i)
            memory.map_word(SystemServices::kBiosVectors[i], SystemServices::bios_stub(i));
        // Case-insensitive PE basenames also work on case-sensitive development hosts.
        std::map<std::string, std::filesystem::path> files;
        for (const auto& item : std::filesystem::directory_iterator(directory)) {
            if (!item.is_regular_file())
                continue;
            auto ext = item.path().extension().string();
            for (auto& c : ext)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c + 32);
            if (ext != ".dll" && ext != ".exe")
                continue;
            const auto physical = std::filesystem::canonical(item.path());
            if (physical.parent_path() != directory)
                continue;
            const auto name = Modules::normalize(item.path().filename().string());
            if (!files.emplace(name, physical).second)
                throw PeError("ambiguous module basename: " + name);
        }
        // --real-directx: the disc's own DDRAW/DSOUND run (on emulated hardware) instead of the
        // guest-COM replacements; only COREDLL, the kernel's interface, stays replaced.
        const std::set<std::string> replaced =
            real_directx ? std::set<std::string>{"coredll.dll"}
                         : std::set<std::string>{"coredll.dll", "ddraw.dll", "dsound.dll"};
        std::map<std::pair<std::string, std::uint16_t>, std::string> ordinals;
        for (const auto& name : replaced) {
            const auto it = files.find(name);
            if (it == files.end())
                continue;
            const auto metadata = prepare_pe(read_file(it->second));
            for (const auto& e : metadata.exports)
                if (!e.name.empty())
                    ordinals.emplace(std::make_pair(name, static_cast<std::uint16_t>(e.ordinal)),
                                     e.name);
        }
        auto named = [&](Import i) {
            i.dll = Modules::normalize(i.dll);
            if (i.ordinal) {
                const auto it = ordinals.find({i.dll, *i.ordinal});
                if (it != ordinals.end()) {
                    i.name = it->second;
                    i.ordinal.reset();
                }
            }
            return i;
        };
        // --lazy: an unsupported import of a replaced DLL links to its own stub address. Calling
        // it stops the run with the function's name, so the order a title really needs its
        // services in shows up one at a time instead of every static import blocking the link.
        constexpr std::uint32_t kPending = 0x1F200000;
        std::vector<std::string> pending;
        // DREAM_WINCE_PROFILE: also the host time each service takes and how often it is called,
        // by the name it was imported under (a blocking wait counts the time other threads ran
        // meanwhile), and the time the console hardware takes.
        const char* profile_text = std::getenv("DREAM_WINCE_PROFILE");
        const bool profile = profile_text != nullptr;
        struct ServiceTime {
            std::uint64_t calls = 0;
            std::chrono::steady_clock::duration time{};
        };
        std::map<std::uint32_t, std::string> service_names;
        std::map<std::uint32_t, ServiceTime> service_time;
        std::chrono::steady_clock::duration hardware_time{};
        std::uint64_t services_called = 0;  // for telling a busy-wait from work
        ImportResolver hle = [&](const Import& request) -> std::optional<std::uint32_t> {
            const auto i = named(request);
            if (auto target = kernel.resolve(i)) {
                service_names.emplace(*target,
                                      i.ordinal ? "#" + std::to_string(*i.ordinal) : i.name);
                return target;
            }
            if (!real_directx)  // otherwise DDRAW/DSOUND imports bind to the disc's modules
                if (auto target = dx.resolve(i))
                    return target;
            if (!lazy || !replaced.contains(i.dll))
                return {};
            const auto label =
                i.dll + "!" + (i.ordinal ? "#" + std::to_string(*i.ordinal) : i.name);
            const auto it = std::find(pending.begin(), pending.end(), label);
            const auto index = static_cast<std::uint32_t>(it - pending.begin());
            if (it == pending.end())
                pending.push_back(label);
            return kPending + index * 4;
        };
        auto provider = [&](const std::string& name) {
            const auto it = files.find(name);
            if (it == files.end())
                throw PeError("module file not found: " + name);
            return read_file(it->second);
        };
        const auto root = Modules::normalize(path.filename().string());
        if (mode == "--audit") {
            std::set<std::string> visited, unresolved;
            std::function<void(const std::string&, const Image&)> visit;
            visit = [&](const std::string& name, const Image& im) {
                if (!visited.insert(name).second)
                    return;
                if (visited.size() > 128)
                    throw PeError("audit module limit exceeded");
                std::cout << "module " << name << " imports=" << im.imports.size()
                          << " exports=" << im.exports.size() << '\n';
                for (const auto& request : im.imports) {
                    if (hle(request))
                        continue;
                    const auto s = named(request);
                    if (replaced.contains(s.dll)) {
                        unresolved.insert(s.dll + "!" +
                                          (s.ordinal ? "#" + std::to_string(*s.ordinal) : s.name));
                    } else {
                        try {
                            const auto dep = prepare_pe(provider(s.dll));
                            const bool exported = std::any_of(
                                dep.exports.begin(), dep.exports.end(), [&](const Export& e) {
                                    return s.ordinal ? e.ordinal == *s.ordinal : e.name == s.name;
                                });
                            if (!exported)
                                unresolved.insert(
                                    s.dll + "!" +
                                    (s.ordinal ? "#" + std::to_string(*s.ordinal) : s.name));
                            visit(s.dll, dep);
                        } catch (const PeError& e) {
                            unresolved.insert(s.dll + ": " + e.what());
                        }
                    }
                }
            };
            visit(root, prepare_pe(file, base));
            for (const auto& name : unresolved) std::cout << "unsupported " << name << '\n';
            std::cout << "modules=" << visited.size() << " unsupported=" << unresolved.size()
                      << " (static import inventory; initialization and forwarded exports are not "
                         "executed)\n";
            return unresolved.empty() ? 0 : 3;
        }
        Modules modules(memory, provider, hle);
        std::uint32_t pseudo = 0x1E000000;
        for (const auto& name : replaced) {
            modules.register_hle(name, pseudo);
            pseudo += 0x10000;
        }
        const auto main_module = modules.load(root, file, base);
        const auto& image = modules.all().at(root).image;
        std::cout << "PE32 SH-4 loaded: base=0x" << std::hex << main_module << " entry=0x"
                  << image.entry << std::dec << " modules=" << modules.all().size()
                  << " imports=" << image.imports.size() << "\n";
        if (lazy)
            std::cout << "lazy stubs=" << pending.size() << "\n";
        if (mode == "--check")
            return 0;
#ifdef DREAM_DEV_INTERPRETER
        if (image.dll)
            throw std::runtime_error("standalone DLL execution is not supported");
        const auto stack = memory.allocate(65536);
        if (!stack)
            throw std::runtime_error("cannot allocate guest stack");
        cpu.pc = image.entry;
        cpu.pr = 0x1EFFFFFC;
        cpu.r[15] = stack + 65536 - 16;
        cpu.next_event = ~0ull;
        // Never deleted: other threads' host threads may still be waiting when main returns.
        Scheduler* scheduler = nullptr;
        dream::devinterp::Options options;
        options.call_translated = false;
        options.max_instructions = steps;
        // DREAM_WINCE_WATCH=addr[,addr...]: print the caller of every call to these addresses.
        std::vector<std::uint32_t> watched;
        if (const char* w = std::getenv("DREAM_WINCE_WATCH"))
            for (std::string_view rest(w); !rest.empty();) {
                const auto comma = rest.find(',');
                watched.push_back(number(rest.substr(0, comma)));
                rest = comma == rest.npos ? std::string_view() : rest.substr(comma + 1);
            }
        // Returns to each call site, for finding busy-waits (below, at options.on_return).
        struct Turn {  // the last return to a call site
            std::uint64_t key = ~0ull, cycles = 0, writes = 0, reads = 0, services = 0;
            std::uint32_t r0 = 0, repeats = 0;
            std::array<std::uint32_t, 8> preserved{};  // r8..r15
            std::array<std::uint32_t, 4> call_args{};  // r4..r7 at the last call from the site
            bool looped = false;  // that call came straight back, with the same arguments
        };
        std::array<Turn, 256> turns{};
        auto turn_key = [](std::uint32_t thread, std::uint32_t site) {
            return std::uint64_t{thread} << 32 | site;
        };
        auto turn_at = [&](std::uint32_t thread, std::uint32_t site) -> Turn& {
            return turns[((site >> 1) ^ (thread * 0x9E37u)) & (turns.size() - 1)];
        };
        // Per thread: the call site the last return went to, while no call has been made since.
        struct LastReturn {
            std::uint32_t site = 0;
            bool clean = false;
        };
        std::array<LastReturn, 64> last_return{};
        options.hle_call = [&](auto& c, auto& m, auto target) {
            // A call from the site the last return went to, with no call in between: a loop
            // asking the same function again.
            if (scheduler) {
                const std::uint32_t thread = scheduler->current_id();
                auto& last = last_return[thread & (last_return.size() - 1)];
                if (last.clean && c.pr == last.site)
                    if (Turn& turn = turn_at(thread, c.pr); turn.key == turn_key(thread, c.pr)) {
                        const std::array<std::uint32_t, 4> args{c.r[4], c.r[5], c.r[6], c.r[7]};
                        turn.looped = args == turn.call_args;
                        turn.call_args = args;
                    }
                last.clean = false;
            }
            if (std::find(watched.begin(), watched.end(), target) != watched.end())
                std::fprintf(stderr,
                             "watch: thread %u @%llu call 0x%08x from 0x%08x (r4=0x%08x "
                             "r5=0x%08x r6=0x%08x r7=0x%08x r8=0x%08x r9=0x%08x r10=0x%08x "
                             "r11=0x%08x r12=0x%08x r13=0x%08x r14=0x%08x)\n",
                             scheduler ? scheduler->current_id() : 0u,
                             static_cast<unsigned long long>(c.cycles / 200000), target, c.pr,
                             c.r[4], c.r[5], c.r[6], c.r[7], c.r[8], c.r[9], c.r[10], c.r[11],
                             c.r[12], c.r[13], c.r[14]);
            // Every call and computed jump the guest makes comes here: the services all sit at
            // 0x1Fxxxxxx, so anything else is guest code, at once.
            if ((target >> 24) != 0x1F)
                return false;
            if (target >= kPending && target < kPending + pending.size() * 4 && !(target & 3u)) {
                char args[160];
                std::snprintf(args, sizeof args,
                              " (r4=0x%08x r5=0x%08x r6=0x%08x r7=0x%08x, caller 0x%08x)", c.r[4],
                              c.r[5], c.r[6], c.r[7], c.pr);
                std::string caller;
                for (const auto& [name, module] : modules.all())
                    if (c.pr >= module.image.base &&
                        c.pr - module.image.base < module.image.bytes.size()) {
                        char at[32];
                        std::snprintf(at, sizeof at, "+0x%x", c.pr - module.image.base);
                        caller = " in " + name + at;
                    }
                // Arguments that point at readable UTF-16 text are shown as text too.
                std::string text;
                for (unsigned r = 4; r < 8; ++r) {
                    std::string s;
                    try {
                        for (std::uint32_t p = c.r[r]; s.size() < 80 && !(p & 1u); p += 2) {
                            const auto ch = m.read16(p);
                            if (!ch || ch > 126 || ch < 32)
                                break;
                            s += static_cast<char>(ch);
                        }
                    } catch (const std::exception&) {
                    }
                    if (s.size() < 2) {  // not UTF-16: try bytes
                        s.clear();
                        try {
                            for (std::uint32_t p = c.r[r]; s.size() < 80; ++p) {
                                const auto ch = m.read8(p);
                                if (!ch || ch > 126 || ch < 32)
                                    break;
                                s += static_cast<char>(ch);
                            }
                        } catch (const std::exception&) {
                        }
                    }
                    if (s.size() >= 2)
                        text += " r" + std::to_string(r) + "=\"" + s + "\"";
                }
                throw std::runtime_error("unimplemented import called: " +
                                         pending[(target - kPending) / 4] + args + caller + text);
            }
            ++services_called;
            if (!profile)
                return kernel.invoke(c, m, target) || dx.invoke(c, m, target);
            const auto start = std::chrono::steady_clock::now();
            const bool served = kernel.invoke(c, m, target) || dx.invoke(c, m, target);
            auto& spent = service_time[target];  // after the call: a wait lets others in
            ++spent.calls;
            spent.time += std::chrono::steady_clock::now() - start;
            return served;
        };
        // Busy-waits on the graphics hardware. DirectDraw answers DDERR_WASSTILLDRAWING while the
        // hardware has not caught up (a flip waits for the vertical blank, a lock for a render),
        // and titles ask again at once, in a loop: Sega Rally 2's `while (FAILED(d3d->Flip()));`
        // took two thirds of its instructions. Nothing changes the answer before the hardware's
        // next interrupt, so when one call site gets it twice in quick succession the devices run
        // on to that interrupt (a frame at most, never past a sleeping thread's wake-up) instead
        // of the loop: the title gets the same answers, after fewer polls.
        //
        // The same goes for a loop that only reads: Sega Rally 2's races wait for the next frame
        // with `do n = GetFrameCount(); while (n == last);`, the count kept by the vertical-blank
        // interrupt thread. When a call site gets the same answer three times in quick
        // succession, each time called again straight after the previous return (a loop, not
        // a helper several callers share: its vector maths looked the same from inside), the
        // caller's preserved registers (r8 to r15) unchanged, and nothing was written outside
        // the thread's stack, no service called and no hardware register read in between,
        // nothing the loop looks at can change before something else runs: the devices run on
        // to their next interrupt likewise. --no-wait-skip runs the loops as they are.
        constexpr std::uint32_t kStillDrawing = 0x8876021C;
        constexpr std::uint64_t kTightLoop = 20'000, kFrameCycles = 200'000'000 / 60;
        std::uint64_t wait_skips = 0, poll_skips = 0, cycles_skipped = 0;
        // DREAM_WINCE_NO_POLL_SKIP: only the DirectDraw rule, to compare.
        const bool poll_skip = !std::getenv("DREAM_WINCE_NO_POLL_SKIP");
        std::map<std::uint32_t, std::uint64_t> polled;  // call site -> cycles skipped (profile)
        auto skip_to_interrupt = [&](dream::sh4::Ctx& ctx) {
            const auto start = ctx.cycles;
            auto limit = start + kFrameCycles;
            if (const auto wake = scheduler->next_wake())
                limit = std::min(limit, std::max(*wake, start));
            auto& events = hardware.system().sched;
            auto now = start;
            for (;;) {
                now = std::max(now, std::min(events.next_deadline(), limit));
                hardware.advance_to(now);
                const auto& holly = hardware.system().holly;
                if (kernel.deliver_interrupts(holly.istnrm, holly.istext, holly.isterr, true) ||
                    now >= limit)
                    break;
            }
            ctx.cycles = now;
            cycles_skipped += now - start;
            scheduler->preempt(Scheduler::kQuantum);  // the woken interrupt thread runs
        };
        if (!no_wait_skip)
            options.on_return = [&](dream::sh4::Ctx& ctx, std::uint32_t to) {
                if (!scheduler)
                    return;
                const std::uint32_t thread = scheduler->current_id();
                const auto key = turn_key(thread, to);
                Turn& turn = turn_at(thread, to);
                std::array<std::uint32_t, 8> preserved;
                std::copy_n(&ctx.r[8], 8, preserved.begin());
                const bool soon = turn.key == key && ctx.cycles - turn.cycles < kTightLoop;
                const bool same = soon && ctx.r[0] == turn.r0;
                const bool quiet = same && turn.looped && preserved == turn.preserved &&
                                   memory.outside_writes() == turn.writes &&
                                   memory.device_reads() == turn.reads &&
                                   services_called == turn.services;
                const std::uint32_t repeats = quiet ? turn.repeats + 1 : 0;
                if (same && ctx.r[0] == kStillDrawing) {
                    ++wait_skips;
                    skip_to_interrupt(ctx);
                } else if (repeats >= 2 && poll_skip) {
                    ++poll_skips;
                    const auto before = ctx.cycles;
                    skip_to_interrupt(ctx);
                    if (profile)
                        polled[to] += ctx.cycles - before;
                }
                if (turn.key != key)
                    turn = Turn{};
                turn.key = key;
                turn.cycles = ctx.cycles;
                turn.writes = memory.outside_writes();
                turn.reads = memory.device_reads();
                turn.services = services_called;
                turn.r0 = ctx.r[0];
                turn.repeats = repeats;
                turn.preserved = preserved;
                turn.looped = false;
                last_return[thread & (last_return.size() - 1)] = {to, true};
                // What the functions it calls write below its stack frame is theirs; a write to
                // the caller's own frame (a loop counter kept there) is work.
                memory.set_quiet_window(ctx.r[15] - 0x2000, 0x2000);
            };
        // Every run of guest code goes in time slices, nested ones (DLL entry points, window
        // procedures) too, so another thread can run while this one sits in a callback.
        constexpr std::uint32_t kSlice = 50000;
        dream::devinterp::Options sliced = options;
        sliced.max_instructions = kSlice;
        std::uint64_t executed = 0, thread_instructions = 0;
        std::atomic<std::uint64_t> progress{0};  // what the watchdog watches
        // DREAM_WINCE_PROFILE=N: where the time goes, the PC where each run of guest code
        // stopped (a time slice ends anywhere) weighed by the instructions it ran, in buckets of
        // N bytes (256 when N is 1 or not a power of two), the busiest printed at the end. 16
        // finds the loops.
        std::uint32_t bucket = profile ? std::strtoul(profile_text, nullptr, 0) : 256;
        if (bucket < 2 || (bucket & (bucket - 1)))
            bucket = 256;
        std::map<std::uint32_t, std::uint64_t> samples;
        auto run_sliced = [&](dream::sh4::Ctx& ctx, std::uint32_t return_to,
                              dream::devinterp::Stats& total) {
            for (;;) {
                dream::devinterp::Stats slice;
                const auto stop = dream::devinterp::run(ctx, memory, return_to, sliced, &slice);
                if (profile)  // weighed by what ran: short nested runs end in the same few places
                    samples[ctx.pc & ~(bucket - 1)] += slice.instructions;
                executed += slice.instructions;
                progress.store(executed, std::memory_order_relaxed);
                total.instructions += slice.instructions;
                total.native_calls += slice.native_calls;
                if (stop != dream::devinterp::Stop::StepLimit)
                    return stop;
                if (executed >= steps)
                    throw StepBudget{};
#ifdef DREAM_WINCE_WINDOW
                if (live && live->closed())  // the player closed the window: the run is over
                    throw ProcessExit{0};
#endif
                const auto hardware_start = std::chrono::steady_clock::now();
                hardware.advance_to(ctx.cycles);  // vblanks and renders keep time
                {
                    const auto& holly = hardware.system().holly;
                    kernel.deliver_interrupts(holly.istnrm, holly.istext, holly.isterr, true);
                }
                hardware_time += std::chrono::steady_clock::now() - hardware_start;
                scheduler->preempt(Scheduler::kQuantum);
            }
        };
        // Never deleted: other threads' host threads may still be waiting when main returns.
        // "module+0xoffset" for a guest address, for error messages.
        auto where = [&](std::uint32_t address) {
            for (const auto& [name, module] : modules.all())
                if (address >= module.image.base &&
                    address - module.image.base < module.image.bytes.size()) {
                    char at[32];
                    std::snprintf(at, sizeof at, "+0x%x", address - module.image.base);
                    return name + at;
                }
            char raw[16];
            std::snprintf(raw, sizeof raw, "0x%08x", address);
            return std::string(raw);
        };
        scheduler = new Scheduler([&](dream::sh4::Ctx& ctx) -> std::uint32_t {
            dream::devinterp::Stats thread_stats;
            dream::devinterp::Stop stop;
            try {
                stop = run_sliced(ctx, Scheduler::kThreadReturn, thread_stats);
            } catch (const std::runtime_error& e) {
                // Raised again on the main thread: say which thread and where it was.
                // DREAM_WINCE_REGS: its registers too.
                std::string regs;
                if (std::getenv("DREAM_WINCE_REGS"))
                    for (unsigned i = 0; i < 16; ++i) {
                        char r[20];
                        std::snprintf(r, sizeof r, " r%u=%08x", i, ctx.r[i]);
                        regs += r;
                    }
                throw std::runtime_error(std::string(e.what()) + " [thread " +
                                         std::to_string(scheduler->current_id()) + " at " +
                                         where(ctx.pc) + ", PR " + where(ctx.pr) + regs + "]");
            }
            thread_instructions += thread_stats.instructions;
            if (stop != dream::devinterp::Stop::Returned)
                throw std::runtime_error("guest thread stopped before returning");
            return ctx.r[0];
        });
        scheduler->adopt_main(cpu);
        kernel.set_scheduler(scheduler, &memory);
        // When every thread waits, the devices run on to their next event and interrupt.
        scheduler->device_deadline = [&]() -> std::optional<std::uint64_t> {
            const auto next = hardware.system().sched.next_deadline();
            if (next == dream::sched::kNever)
                return std::nullopt;
            return next;
        };
        scheduler->run_devices = [&](std::uint64_t cycles) {
            hardware.advance_to(cycles);
            const auto& holly = hardware.system().holly;
            return kernel.deliver_interrupts(holly.istnrm, holly.istext, holly.isterr, false);
        };
        // Watchdog: a run that stops executing (threads waiting on each other on the host side)
        // reports every thread's state and ends instead of hanging. DREAM_WINCE_STATUS=N also
        // reports progress every N seconds (a development aid: the report reads the running
        // threads' state without stopping them).
        const char* status_env = std::getenv("DREAM_WINCE_STATUS");
        const unsigned status_every = status_env ? static_cast<unsigned>(std::atoi(status_env)) : 0;
        std::thread([&progress, &memory, &hardware, scheduler, status_every] {
            std::uint64_t last = ~0ull;
            unsigned still = 0, ticks = 0;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                const auto now = progress.load(std::memory_order_relaxed);
                if (status_every && ++ticks % status_every == 0)
                    std::fprintf(stderr,
                                 "status: %llu instructions, memory %u KiB in %u blocks, maple "
                                 "transfers %llu frames %llu\n%s",
                                 static_cast<unsigned long long>(now), memory.used() >> 10,
                                 memory.allocations(),
                                 static_cast<unsigned long long>(hardware.maple().transfers),
                                 static_cast<unsigned long long>(hardware.maple().frames),
                                 scheduler->describe().c_str());
                still = now == last ? still + 1 : 0;
                last = now;
                if (still >= 15) {
                    std::fprintf(stderr, "watchdog: no guest instruction for 15 s\n%s",
                                 scheduler->describe().c_str());
                    std::fflush(stderr);
                    std::fflush(stdout);
                    std::_Exit(4);
                }
            }
        }).detach();
        guest_clock = [&] { return scheduler->current_ctx().cycles; };
        // A service calling back into the title (window procedure, timer callback): a nested
        // interpreter run below the caller's frame, returning R0; the caller's state is kept.
        std::uint64_t callback_instructions = 0;
        // Arguments past the fourth go on the stack from SP+16, above the home slots.
        const auto guest_call = [&](std::uint32_t function,
                                    const std::vector<std::uint32_t>& args) {
            auto& ctx = scheduler->current_ctx();
            const auto saved = ctx;
            const auto stacked = static_cast<std::uint32_t>(args.size() > 4 ? args.size() - 4 : 0);
            ctx.r[15] = (ctx.r[15] - 64 - 4 * stacked) & ~3u;
            memory.check(ctx.r[15] - 16, 16 + 4 * stacked, ProcessMemory::Write);
            ctx.r[15] -= 16;  // the four home slots every CE call reserves
            for (std::uint32_t i = 0; i < stacked; ++i)
                memory.write32(ctx.r[15] + 16 + 4 * i, args[4 + i]);
            ctx.pc = function;
            ctx.pr = 0x1EFFFFF0;
            for (unsigned i = 0; i < 4; ++i) ctx.r[4 + i] = i < args.size() ? args[i] : 0;
            // A host service (an HLE trap address) is called directly: the interpreter only
            // recognises one at a call instruction.
            try {
                if (options.hle_call(ctx, memory, function)) {
                    const auto r0 = ctx.r[0];
                    const auto cycles = ctx.cycles;
                    ctx = saved;
                    ctx.cycles = cycles;
                    return r0;
                }
            } catch (...) {
                const auto cycles = ctx.cycles;
                ctx = saved;
                ctx.cycles = cycles;
                throw;
            }
            dream::devinterp::Stats nested;
            try {
                const auto stop = run_sliced(ctx, ctx.pr, nested);
                callback_instructions += nested.instructions;
                if (stop != dream::devinterp::Stop::Returned)
                    throw std::runtime_error("guest callback stopped before returning");
                const auto r0 = ctx.r[0];
                const auto cycles = ctx.cycles;
                ctx = saved;
                ctx.cycles = cycles;
                return r0;
            } catch (...) {
                const auto cycles = ctx.cycles;
                ctx = saved;
                ctx.cycles = cycles;
                throw;
            }
        };
        const Kernel::GuestCall kernel_guest_call = [&](std::uint32_t function,
                                                        std::array<std::uint32_t, 4> args) {
            return guest_call(function, {args.begin(), args.end()});
        };
        kernel.set_guest_call(kernel_guest_call);
        // COM classes: ask each DLL beside the executable that exports DllGetClassObject,
        // MGame* engine DLLs first, for an IClassFactory of the class. Set up below, once the
        // DLL entry call exists.
        std::function<std::optional<std::string>(const std::array<std::uint8_t, 16>&)> resolver;
        std::uint64_t dll_instructions = 0;
        Modules::EntryCall enter = [&](std::uint32_t entry, std::uint32_t instance,
                                       unsigned reason) {
            auto& ctx = scheduler->current_ctx();
            const auto saved = ctx;
            if (ctx.r[15] < 16)
                throw PeError("DLL call stack underflow");
            memory.check(ctx.r[15] - 16, 16, ProcessMemory::Write);
            ctx.r[15] -= 16;
            ctx.pc = entry;
            ctx.pr = 0x1EFFFFF8;
            ctx.r[4] = instance;
            ctx.r[5] = reason;
            ctx.r[6] = 0;
            dream::devinterp::Stats dll_stats;
            try {
                const auto stop = run_sliced(ctx, ctx.pr, dll_stats);
                const bool ok = stop == dream::devinterp::Stop::Returned && ctx.r[0] != 0;
                const auto cycles = ctx.cycles;
                ctx = saved;
                ctx.cycles = cycles;
                dll_instructions += dll_stats.instructions;
                if (stop != dream::devinterp::Stop::Returned)
                    throw PeError("DLL initializer stopped before return");
                return ok;
            } catch (...) {
                const auto cycles = ctx.cycles;
                ctx = saved;
                ctx.cycles = cycles;
                throw;
            }
        };
        kernel.attach_modules(modules, main_module, enter);
        std::vector<std::string> servers;
        for (const auto& [name, file_path] : files) {
            if (replaced.contains(name) || name == root)
                continue;
            try {
                const auto metadata = prepare_pe(read_file(file_path));
                if (metadata.dll &&
                    std::any_of(metadata.exports.begin(), metadata.exports.end(),
                                [](const Export& e) { return e.name == "DllGetClassObject"; }))
                    servers.push_back(name);
            } catch (const PeError&) {
                // not an SH-4 CE module (discs carry PC tools too)
            }
        }
        std::stable_partition(servers.begin(), servers.end(),
                              [](const std::string& n) { return n.starts_with("mgame"); });
        std::uint32_t probe = 0;
        resolver = [&](const std::array<std::uint8_t, 16>& clsid) -> std::optional<std::string> {
            if (!probe && !(probe = memory.allocate(4096)))
                throw std::runtime_error("no guest memory for COM class probes");
            // IID_IClassFactory {00000001-0000-0000-C000-000000000046}
            static constexpr std::array<std::uint8_t, 16> factory{1,    0, 0, 0, 0, 0, 0, 0,
                                                                  0xC0, 0, 0, 0, 0, 0, 0, 0x46};
            memory.write_bytes(probe, clsid);
            memory.write_bytes(probe + 16, factory);
            for (const auto& name : servers) {
                try {
                    const auto handle = modules.load_library(name);
                    modules.initialize(enter);
                    Import entry;
                    entry.name = "DllGetClassObject";
                    const auto function = modules.procedure(handle, entry);
                    memory.write32(probe + 32, 0);
                    const auto hr = kernel_guest_call(function, {probe, probe + 16, probe + 32, 0});
                    const auto object = memory.read32(probe + 32);
                    if (hr != 0 || !object)
                        continue;
                    const auto release = memory.read32(memory.read32(object) + 8);
                    kernel_guest_call(release, {object, 0, 0, 0});
                    return name;
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "wince: COM probe of %s failed: %s\n", name.c_str(),
                                 e.what());
                }
            }
            return std::nullopt;
        };
        kernel.set_class_resolver(resolver);
        modules.initialize(enter);
        // --register: what installing the title did. Each DLL beside the executable that exports
        // DllRegisterServer writes its COM classes into the registry, so OLE32 finds them.
        if (register_servers)
            for (const auto& [name, file_path] : files) {
                if (replaced.contains(name) || name == root)
                    continue;
                const auto metadata = prepare_pe(read_file(file_path));
                if (!metadata.dll ||
                    std::none_of(metadata.exports.begin(), metadata.exports.end(),
                                 [](const Export& e) { return e.name == "DllRegisterServer"; }))
                    continue;
                const auto handle = modules.load_library(name);
                modules.initialize(enter);
                Import entry;
                entry.name = "DllRegisterServer";
                const auto function = modules.procedure(handle, entry);
                const auto hr = kernel_guest_call(function, {0, 0, 0, 0});
                std::fprintf(stderr, "register: %s -> 0x%08x\n", name.c_str(), hr);
            }
        // The disc's drivers on the emulated console, hosted as WDEVICE.EXE hosts them: WDMLIB
        // loads each one registered under HKLM\WDMDrivers\BuiltIn. So far the Maple bus driver
        // (pads, memory cards); the GD-ROM driver (WSEGACD.DLL) needs a GD-ROM drive first.
        WdmHost wdm(memory, guest_call,
                    [&](const std::string& dll, const std::string& name) -> std::uint32_t {
                        Import symbol;
                        symbol.dll = dll;
                        symbol.name = name;
                        if (dll == "coredll.dll")
                            return kernel.resolve(symbol).value_or(0);
                        try {
                            const auto handle = modules.load_library(dll);
                            modules.initialize(enter);
                            return modules.procedure(handle, symbol);
                        } catch (const std::exception&) {
                            return 0;
                        }
                    });
        if (real_directx && files.contains("wdmlib.dll") && files.contains("maple.dll")) {
            auto* system = kernel.system_services();
            std::vector<std::uint8_t> dll;
            for (const char16_t c : std::u16string(u"maple.dll")) {
                dll.push_back(static_cast<std::uint8_t>(c));
                dll.push_back(static_cast<std::uint8_t>(c >> 8));
            }
            dll.insert(dll.end(), {0, 0});
            const std::u16string key = u"HKEY_LOCAL_MACHINE\\WDMDrivers\\BuiltIn\\Maple";
            system->set_value(key, u"Dll", 1 /* REG_SZ */, dll);
            system->set_value(key, u"Order", 4 /* REG_DWORD */, {0, 0, 0, 0});
            wdm.clock = [&] { return scheduler->current_ctx().cycles; };
            if (wdm.start()) {
                kernel.set_device_host(&wdm);
                std::fprintf(stderr, "wince: driver host started (maple.dll)\n");
                // On the console WDEVICE starts at boot, seconds before the title, so every pad
                // and card is found before the title looks (the bus is scanned once a frame and
                // a card shows up frames after its pad). A title that sees devices arrive while
                // it starts re-creates its objects with requests still pending. One second of
                // guest time lets the driver find everything first.
                scheduler->wait([] { return false; }, cpu.cycles + 200'000'000ull);
            }
        }
        // --disc: the disc image the title came from, for what it asks the CD-ROM device (its
        // table of contents, CD audio) while no GD-ROM driver runs.
        if (!disc_image.empty()) {
            std::string error;
            const auto disc = dream::gdrom::open_disc(disc_image, error);
            if (!disc)
                throw std::invalid_argument("--disc: " + error);
            std::vector<SystemServices::CdTrack> tracks;
            for (const auto& t : disc->tracks()) tracks.push_back({t.number, t.lba, t.data});
            kernel.system_services()->set_cd_tracks(std::move(tracks), disc->leadout_lba());
            std::fprintf(stderr, "wince: disc %s, %zu tracks\n", disc_image.c_str(),
                         disc->tracks().size());
#ifdef DREAM_WINCE_AUDIO
            // The drive plays CD audio by itself: a second reader of the image, on a thread of
            // its own.
            if (audio && effects.is_open()) {
                if (auto own = dream::gdrom::open_disc(disc_image, error)) {
                    cd_audio = std::make_unique<CdAudio>(std::move(own));
                    if (cd_audio->start(error))
                        kernel.system_services()->set_cd_audio(cd_audio.get());
                    else {
                        std::fprintf(stderr, "wince: no CD audio: %s\n", error.c_str());
                        cd_audio.reset();
                    }
                }
            }
#endif
        }
        // DREAM_WINCE_MODULES: where every module sits, to read addresses in later messages.
        if (std::getenv("DREAM_WINCE_MODULES"))
            for (const auto& [name, module] : modules.all())
                std::fprintf(
                    stderr, "module %-20s 0x%08x-0x%08x\n", name.c_str(), module.image.base,
                    module.image.base + static_cast<std::uint32_t>(module.image.bytes.size()));
        dream::devinterp::Stats stats;
        auto result = dream::devinterp::Stop::Returned;
        const auto run_start = std::chrono::steady_clock::now();
        try {
            result = run_sliced(cpu, cpu.pr, stats);
        } catch (const ProcessExit& exit) {
            std::cout << "Guest ExitProcess(" << exit.code << ")\n";
        } catch (const StepBudget&) {
            result = dream::devinterp::Stop::StepLimit;
        } catch (const std::runtime_error& e) {
            save_last();
            // DREAM_WINCE_PEEK=addr[,addr]: the words at those process addresses when it failed.
            if (const char* peek = std::getenv("DREAM_WINCE_PEEK"))
                for (std::string_view rest(peek); !rest.empty();) {
                    const auto comma = rest.find(',');
                    const auto address = number(rest.substr(0, comma));
                    try {
                        std::fprintf(stderr, "peek 0x%08x = 0x%08x\n", address,
                                     memory.read32(address));
                    } catch (const std::exception&) {
                        std::fprintf(stderr, "peek 0x%08x: unreadable\n", address);
                    }
                    rest = comma == rest.npos ? std::string_view() : rest.substr(comma + 1);
                }
            // DREAM_WINCE_MODULES: where every module sits, to read addresses in the message.
            if (std::getenv("DREAM_WINCE_MODULES"))
                for (const auto& [name, module] : modules.all())
                    std::fprintf(
                        stderr, "module %-20s 0x%08x-0x%08x\n", name.c_str(), module.image.base,
                        module.image.base + static_cast<std::uint32_t>(module.image.bytes.size()));
            const std::string what = e.what();
            if (what.find("[thread ") != std::string::npos)
                throw;
            // DREAM_WINCE_REGS: its registers too, as for the other threads.
            std::string regs;
            if (std::getenv("DREAM_WINCE_REGS"))
                for (unsigned i = 0; i < 16; ++i) {
                    char r[20];
                    std::snprintf(r, sizeof r, " r%u=%08x", i, cpu.r[i]);
                    regs += r;
                }
            throw std::runtime_error(what + " [main thread at " + where(cpu.pc) + ", PR " +
                                     where(cpu.pr) + regs + "]");
        }
        save_last();
        std::cout << "instructions=" << stats.instructions << " HLE calls=" << stats.native_calls
                  << " DLL instructions=" << dll_instructions << " r0=" << cpu.r[0] << "\n";
        std::cout << "video: frames=" << hardware.system().spg.frames() << " renders=" << renders
                  << " device accesses=" << hardware.accesses() << "\n";
        if (wait_skips || poll_skips)
            std::cout << "busy-waits skipped=" << wait_skips << " polls skipped=" << poll_skips
                      << " guest time skipped="
                      << cycles_skipped * 100 / std::max<std::uint64_t>(cpu.cycles, 1) << "%\n";
#ifdef DREAM_WINCE_WINDOW
        if (live)
            std::cout << "window: drawn=" << live->renders() << " shown=" << live->presented()
                      << "\n";
#endif
        std::cout << "main thread at " << where(cpu.pc) << ", PR " << where(cpu.pr) << "\n";
        if (std::getenv("DREAM_WINCE_MODULES"))
            for (const auto& [name, module] : modules.all()) {
                char range[40];
                std::snprintf(
                    range, sizeof range, "0x%08x-0x%08x", module.image.base,
                    module.image.base + static_cast<std::uint32_t>(module.image.bytes.size()));
                std::cout << "module " << name << " " << range << "\n";
            }
        if (profile) {
            std::vector<std::pair<std::uint64_t, std::uint32_t>> busiest;
            for (const auto& [pc, n] : samples) busiest.emplace_back(n, pc);
            std::sort(busiest.rbegin(), busiest.rend());
            std::uint64_t all = 0;
            for (const auto& [n, pc] : busiest) all += n;
            for (std::size_t i = 0; i < busiest.size() && i < 25; ++i)
                std::cout << "profile: " << where(busiest[i].second) << " "
                          << busiest[i].first * 100 / std::max<std::uint64_t>(all, 1) << "%\n";
            const auto seconds = [](std::chrono::steady_clock::duration d) {
                return std::chrono::duration<double>(d).count();
            };
            std::chrono::steady_clock::duration services{};
            std::vector<std::pair<std::chrono::steady_clock::duration, std::uint32_t>> costliest;
            for (const auto& [target, spent] : service_time) {
                services += spent.time;
                costliest.emplace_back(spent.time, target);
            }
            std::sort(costliest.rbegin(), costliest.rend());
            std::printf("profile: host %.1f s, of which services %.1f s, hardware %.1f s\n",
                        seconds(std::chrono::steady_clock::now() - run_start), seconds(services),
                        seconds(hardware_time));
            for (const auto& [site, skipped] : polled)
                std::printf("profile: polls returning to %s: %.3f s of guest time skipped\n",
                            where(site).c_str(), static_cast<double>(skipped) / 200e6);
            std::printf("profile: slow memory accesses: reads=%llu writes=%llu fetches=%llu\n",
                        static_cast<unsigned long long>(memory.slow_reads),
                        static_cast<unsigned long long>(memory.slow_writes),
                        static_cast<unsigned long long>(memory.slow_fetches));
            for (std::size_t i = 0; i < costliest.size() && i < 15; ++i) {
                const auto target = costliest[i].second;
                const auto name = service_names.find(target);
                std::printf(
                    "profile: service %-28s calls=%-9llu %.2f s\n",
                    name != service_names.end() ? name->second.c_str() : where(target).c_str(),
                    static_cast<unsigned long long>(service_time[target].calls),
                    seconds(costliest[i].first));
            }
            std::fflush(stdout);
        }
        std::cout << "sound: ARM7 " << (hardware.aica().arm.enabled() ? "running" : "held")
                  << " instructions=" << hardware.aica().arm.instructions
                  << " samples=" << hardware.aica().samples << " sound RAM in use="
                  << std::count_if(hardware.system().memory.aram(),
                                   hardware.system().memory.aram() + (2u << 20),
                                   [](std::uint8_t b) { return b != 0; })
                  << " bytes\n";
#ifdef DREAM_WINCE_AUDIO
        if (effects.is_open())
            std::cout << "heard: effects samples=" << effects.pushed << " blocks=" << effects.blocks
                      << " gaps=" << effects.starved << " dropped=" << effects.dropped
                      << " CD sectors=" << (cd_audio ? cd_audio->sectors_played() : 0) << "\n";
#endif
        std::cout << "G2 DMA: transfers=" << hardware.g2().transfers
                  << " bytes=" << hardware.g2().bytes << " refused=" << hardware.g2().refused
                  << " to sound RAM=" << hardware.g2().to_aica_ram << "\n";
        std::cout << "store queues: bursts mapped=" << hardware.system().memory.sq_tlb_flushes
                  << " unmapped=" << hardware.system().memory.sq_tlb_misses << "\n";
        std::cout << "maple: transfers=" << hardware.maple().transfers
                  << " frames=" << hardware.maple().frames
                  << " pad reads=" << hardware.pad().condition_reads
                  << " card reads=" << hardware.card().block_reads
                  << " writes=" << hardware.card().block_writes << "\n";
        if (wdm.opens || wdm.controls)
            std::cout << "drivers: opens=" << wdm.opens << " requests=" << wdm.controls
                      << " transfers=" << wdm.transfers << " failed=" << wdm.failures << "\n";
        std::cout << "interrupts:";
        for (const auto& [id, count] : kernel.interrupt_counts())
            std::cout << " SYSINTR " << id << "=" << count;
        std::cout << "\n";
        if (scheduler->created())
            std::cout << "threads created=" << scheduler->created()
                      << " thread instructions=" << thread_instructions << "\n";
        if (callback_instructions)
            std::cout << "callback instructions=" << callback_instructions << "\n";
        if (!dx.diagnostic().empty())
            std::cout << dx.diagnostic() << '\n';
        if (capture_dir)
            capture(dx, *capture_dir);
        if (result != dream::devinterp::Stop::Returned) {
            std::cerr
                << "Execution stopped before return (step limit or unsupported exception flow).\n";
            return 3;
        }
        return 0;
#else
        throw std::runtime_error("--run requires DREAM_DEV_INTERPRETER=ON");
#endif
    } catch (const ProcessExit& exit) {
        std::cout << "Guest ExitProcess(" << exit.code << ")\n";
        return 0;
    } catch (const StepBudget&) {
        std::cerr << "WinCE: step limit reached before the program started\n";
        return 3;
    } catch (const std::exception& e) {
        std::cerr << "WinCE: " << e.what() << " (guest PC=0x" << std::hex << cpu.pc << ")\n";
        return 2;
    }
}
