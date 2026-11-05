#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string_view>

#include "dream/runtime/wince/directx.h"
#include "dream/runtime/wince/media.h"
#include "dream/runtime/wince/modules.h"
#include "dream/runtime/wince/process.h"
#ifdef DREAM_DEV_INTERPRETER
#include "dream/runtime/devinterp/interpreter.h"
#endif

namespace {
using namespace dream::wince;
void help() {
    std::cout
        << "Experimental Windows CE / SH-4 development tools (not a game launcher)\n"
           "dream-wince --check module.exe [--base 0x10000]\n"
           "dream-wince --run module.exe [--base 0x10000] [--steps 100000] [--capture directory]\n"
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
        for (int i = 3; i < argc; i += 2) {
            if (i + 1 == argc)
                throw std::invalid_argument("missing option value");
            const std::string_view option = argv[i];
            if (option == "--base")
                base = number(argv[i + 1]);
            else if (option == "--steps")
                steps = number(argv[i + 1]);
            else if (option == "--capture" && mode == "--run")
                capture_dir = argv[i + 1];
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
        DirectX dx(memory);
        const auto path = std::filesystem::absolute(argv[2]);
        const auto directory = std::filesystem::canonical(path.parent_path());
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
        const std::set<std::string> replaced{"coredll.dll", "ddraw.dll", "dsound.dll"};
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
        ImportResolver hle = [&](const Import& request) -> std::optional<std::uint32_t> {
            const auto i = named(request);
            if (auto target = kernel.resolve(i))
                return target;
            return dx.resolve(i);
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
        options.hle_call = [&](auto& c, auto& m, auto target) {
            return kernel.invoke(c, m, target) || dx.invoke(c, m, target);
        };
        std::uint64_t dll_instructions = 0;
        Modules::EntryCall enter = [&](std::uint32_t entry, std::uint32_t instance,
                                       unsigned reason) {
            const auto saved = cpu;
            if (cpu.r[15] < 16)
                throw PeError("DLL call stack underflow");
            memory.check(cpu.r[15] - 16, 16, ProcessMemory::Write);
            cpu.r[15] -= 16;
            cpu.pc = entry;
            cpu.pr = 0x1EFFFFF8;
            cpu.r[4] = instance;
            cpu.r[5] = reason;
            cpu.r[6] = 0;
            dream::devinterp::Stats dll_stats;
            try {
                const auto stop = dream::devinterp::run(cpu, memory, cpu.pr, options, &dll_stats);
                const bool ok = stop == dream::devinterp::Stop::Returned && cpu.r[0] != 0;
                const auto cycles = cpu.cycles;
                cpu = saved;
                cpu.cycles = cycles;
                dll_instructions += dll_stats.instructions;
                if (stop != dream::devinterp::Stop::Returned)
                    throw PeError("DLL initializer stopped before return");
                return ok;
            } catch (...) {
                cpu = saved;
                throw;
            }
        };
        kernel.attach_modules(modules, main_module, enter);
        modules.initialize(enter);
        dream::devinterp::Stats stats;
        auto result = dream::devinterp::Stop::Returned;
        try {
            result = dream::devinterp::run(cpu, memory, cpu.pr, options, &stats);
        } catch (const ProcessExit& exit) {
            std::cout << "Guest ExitProcess(" << exit.code << ")\n";
        }
        std::cout << "instructions=" << stats.instructions << " HLE calls=" << stats.native_calls
                  << " DLL instructions=" << dll_instructions << " r0=" << cpu.r[0] << "\n";
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
    } catch (const std::exception& e) {
        std::cerr << "WinCE: " << e.what() << " (guest PC=0x" << std::hex << cpu.pc << ")\n";
        return 2;
    }
}
