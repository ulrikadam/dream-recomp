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
#include "dream/runtime/wince/dc_hardware.h"
#include "dream/runtime/wince/directx.h"
#include "dream/runtime/wince/media.h"
#include "dream/runtime/wince/modules.h"
#include "dream/runtime/wince/process.h"
#include "dream/runtime/wince/system_services.h"
#include "dream/runtime/wince/threads.h"
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
        std::uint32_t steps = 100000;
        std::optional<std::filesystem::path> capture_dir;
        std::string dump_ta;
        std::uint64_t dump_ta_at_frame = 0;
        bool lazy = false, register_servers = false, real_directx = false;
        for (int i = 3; i < argc; i += 2) {
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
            else if (option == "--steps")
                steps = number(argv[i + 1]);
            else if (option == "--capture" && mode == "--run")
                capture_dir = argv[i + 1];
            else if (option == "--dump-ta" && mode == "--run")
                dump_ta = argv[i + 1];
            else if (option == "--dump-ta-at-frame" && mode == "--run")
                dump_ta_at_frame = number(argv[i + 1]);
            else
                throw std::invalid_argument("unknown option");
        }
        if (!steps)
            throw std::invalid_argument("step limit must be positive");
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
        std::uint64_t renders = 0;
        bool dumped = false;
        hardware.pvr().on_render = [&](const std::vector<std::uint32_t>& stream) {
            ++renders;
            const auto frame = hardware.system().spg.frames();
            if (dump_ta.empty() || dumped || frame < dump_ta_at_frame)
                return;
            dumped = true;
            auto save = [](const std::string& name, const void* data, std::size_t bytes) {
                if (FILE* f = std::fopen(name.c_str(), "wb")) {
                    std::fwrite(data, 1, bytes, f);
                    std::fclose(f);
                }
            };
            save(dump_ta, stream.data(), stream.size() * 4);
            save(dump_ta + ".vram", hardware.system().memory.vram(),
                 dream::mem::DcMemory::kVramSize);
            save(dump_ta + ".vram.regs", hardware.pvr().reg_block(), 0x2000);
            std::printf("wrote %s: render %llu at frame %llu, %zu parameter words\n",
                        dump_ta.c_str(), static_cast<unsigned long long>(renders),
                        static_cast<unsigned long long>(frame), stream.size());
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
        ImportResolver hle = [&](const Import& request) -> std::optional<std::uint32_t> {
            const auto i = named(request);
            if (auto target = kernel.resolve(i))
                return target;
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
        options.hle_call = [&](auto& c, auto& m, auto target) {
            if (std::find(watched.begin(), watched.end(), target) != watched.end())
                std::fprintf(stderr,
                             "watch: call 0x%08x from 0x%08x (r4=0x%08x r5=0x%08x r6=0x%08x "
                             "r7=0x%08x r8=0x%08x r9=0x%08x r10=0x%08x r11=0x%08x r12=0x%08x "
                             "r13=0x%08x r14=0x%08x)\n",
                             target, c.pr, c.r[4], c.r[5], c.r[6], c.r[7], c.r[8], c.r[9], c.r[10],
                             c.r[11], c.r[12], c.r[13], c.r[14]);
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
            return kernel.invoke(c, m, target) || dx.invoke(c, m, target);
        };
        // Every run of guest code goes in time slices, nested ones (DLL entry points, window
        // procedures) too, so another thread can run while this one sits in a callback.
        constexpr std::uint32_t kSlice = 50000;
        dream::devinterp::Options sliced = options;
        sliced.max_instructions = kSlice;
        std::uint64_t executed = 0, thread_instructions = 0;
        std::atomic<std::uint64_t> progress{0};  // what the watchdog watches
        Scheduler* scheduler = nullptr;
        auto run_sliced = [&](dream::sh4::Ctx& ctx, std::uint32_t return_to,
                              dream::devinterp::Stats& total) {
            for (;;) {
                dream::devinterp::Stats slice;
                const auto stop = dream::devinterp::run(ctx, memory, return_to, sliced, &slice);
                executed += slice.instructions;
                progress.store(executed, std::memory_order_relaxed);
                total.instructions += slice.instructions;
                total.native_calls += slice.native_calls;
                if (stop != dream::devinterp::Stop::StepLimit)
                    return stop;
                if (executed >= steps)
                    throw StepBudget{};
                hardware.advance_to(ctx.cycles);  // vblanks and renders keep time
                {
                    const auto& holly = hardware.system().holly;
                    kernel.deliver_interrupts(holly.istnrm, holly.istext, holly.isterr, true);
                }
                scheduler->yield();
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
                throw std::runtime_error(std::string(e.what()) + " [thread " +
                                         std::to_string(scheduler->current_id()) + " at " +
                                         where(ctx.pc) + ", PR " + where(ctx.pr) + "]");
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
        // reports every thread's state and ends instead of hanging.
        std::thread([&progress, scheduler] {
            std::uint64_t last = ~0ull;
            unsigned still = 0;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                const auto now = progress.load(std::memory_order_relaxed);
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
        const Kernel::GuestCall kernel_guest_call = [&](std::uint32_t function,
                                                        std::array<std::uint32_t, 4> args) {
            auto& ctx = scheduler->current_ctx();
            const auto saved = ctx;
            ctx.r[15] = (ctx.r[15] - 64) & ~3u;
            memory.check(ctx.r[15] - 16, 16, ProcessMemory::Write);
            ctx.r[15] -= 16;  // the four home slots every CE call reserves
            ctx.pc = function;
            ctx.pr = 0x1EFFFFF0;
            for (unsigned i = 0; i < 4; ++i) ctx.r[4 + i] = args[i];
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
        dream::devinterp::Stats stats;
        auto result = dream::devinterp::Stop::Returned;
        try {
            result = run_sliced(cpu, cpu.pr, stats);
        } catch (const ProcessExit& exit) {
            std::cout << "Guest ExitProcess(" << exit.code << ")\n";
        } catch (const StepBudget&) {
            result = dream::devinterp::Stop::StepLimit;
        } catch (const std::runtime_error& e) {
            // DREAM_WINCE_MODULES: where every module sits, to read addresses in the message.
            if (std::getenv("DREAM_WINCE_MODULES"))
                for (const auto& [name, module] : modules.all())
                    std::fprintf(
                        stderr, "module %-20s 0x%08x-0x%08x\n", name.c_str(), module.image.base,
                        module.image.base + static_cast<std::uint32_t>(module.image.bytes.size()));
            const std::string what = e.what();
            if (what.find("[thread ") != std::string::npos)
                throw;
            throw std::runtime_error(what + " [main thread at " + where(cpu.pc) + ", PR " +
                                     where(cpu.pr) + "]");
        }
        std::cout << "instructions=" << stats.instructions << " HLE calls=" << stats.native_calls
                  << " DLL instructions=" << dll_instructions << " r0=" << cpu.r[0] << "\n";
        std::cout << "video: frames=" << hardware.system().spg.frames() << " renders=" << renders
                  << " device accesses=" << hardware.accesses() << "\n";
        std::cout << "main thread at " << where(cpu.pc) << ", PR " << where(cpu.pr) << "\n";
        std::cout << "sound: ARM7 " << (hardware.aica().arm.enabled() ? "running" : "held")
                  << " instructions=" << hardware.aica().arm.instructions
                  << " samples=" << hardware.aica().samples << " sound RAM in use="
                  << std::count_if(hardware.system().memory.aram(),
                                   hardware.system().memory.aram() + (2u << 20),
                                   [](std::uint8_t b) { return b != 0; })
                  << " bytes\n";
        std::cout << "G2 DMA: transfers=" << hardware.g2().transfers
                  << " bytes=" << hardware.g2().bytes << " refused=" << hardware.g2().refused
                  << " to sound RAM=" << hardware.g2().to_aica_ram << "\n";
        std::cout << "store queues: bursts mapped=" << hardware.system().memory.sq_tlb_flushes
                  << " unmapped=" << hardware.system().memory.sq_tlb_misses << "\n";
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
