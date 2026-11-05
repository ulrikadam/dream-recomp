#pragma once
#include <array>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dream/runtime/hle/flash.h"
#include "dream/runtime/wince/process.h"

namespace dream::wince {
// COREDLL services a title needs to start that are not memory, CRT or synchronization: the
// registry first. Single-threaded, like the other HLE services. Registry functions return their
// Win32 error code directly (they do not set the last error), as on CE.
class SystemServices {
public:
    explicit SystemServices(ProcessMemory& memory);
    static std::optional<std::uint32_t> resolve(const std::string& name);
    static bool handles(std::uint32_t target);
    // Dreamcast BIOS syscalls reached through their vectors (kernel-mode DLLs call them):
    // the vector addresses and the stub each one points at.
    static constexpr std::array<std::uint32_t, 6> kBiosVectors{0x8C0000B0, 0x8C0000B4, 0x8C0000B8,
                                                               0x8C0000BC, 0x8C0000C0, 0x8C0000E0};
    static std::uint32_t bios_stub(unsigned index);
    bool invoke(sh4::Ctx& cpu, Memory& memory, std::uint32_t target, std::uint32_t& last_error);

    // In-memory registry. Paths are "HKLM\\Software\\..." with any case; a value's data is raw
    // bytes in guest layout (REG_SZ is UTF-16 with its terminator).
    void set_value(const std::u16string& key, const std::u16string& name, std::uint32_t type,
                   std::vector<std::uint8_t> data);
    bool has_key(const std::u16string& key) const;

    // Files: CE paths map onto this host folder (the disc's files), read only. "\CD-ROM\" and a
    // leading "\" both name its top; nothing outside it can be reached.
    void set_root(std::filesystem::path root) { root_ = std::move(root); }
    // CloseHandle for a file or device handle; false when the handle is not one of these.
    bool close(std::uint32_t handle) { return files_.erase(handle) != 0; }

    // Runs a guest function (a window procedure, a timer callback) with up to four arguments
    // and returns its R0. Windows need it: the system calls into the title.
    using GuestCall = std::function<std::uint32_t(std::uint32_t, std::array<std::uint32_t, 4>)>;
    void set_guest_call(GuestCall call) { guest_call_ = std::move(call); }

    // The registry a title's OS image carried is compressed in its ROM. COM classes are found
    // instead by asking the title's DLLs: given a CLSID (in memory layout), the resolver returns
    // the file name of the DLL whose DllGetClassObject serves it. A miss on
    // HKCR\CLSID\{...}\InprocServer32 asks it once and records the answer as that key.
    using ClassResolver =
        std::function<std::optional<std::string>(const std::array<std::uint8_t, 16>&)>;
    void set_class_resolver(ClassResolver resolver) { class_resolver_ = std::move(resolver); }

    // The OAL's Holly interrupt routing (nk.exe, at 0x8C02E000 in the Dreamcast OS image):
    // given the status registers, the IST events to signal now. A SYSINTR fires once, then waits
    // for InterruptDone, as the OAL masks it until then.
    std::vector<std::uint32_t> due_interrupts(std::uint32_t istnrm, std::uint32_t istext,
                                              std::uint32_t isterr);

private:
    struct WindowClass {
        std::uint32_t procedure = 0, style = 0, instance = 0;
    };
    struct Window {
        std::u16string class_name;
        std::uint32_t procedure = 0, style = 0;
        std::map<std::int32_t, std::uint32_t> longs;  // SetWindowLong slots other than the proc
    };
    struct Message {
        std::uint32_t window, message, wparam, lparam;
    };
    struct Timer {
        std::uint32_t window, id, period, callback;
        std::uint64_t due;  // in milliseconds of guest time
    };
    std::map<std::u16string, WindowClass> classes_;  // by folded name
    std::map<std::uint32_t, Window> windows_;
    std::deque<Message> posted_;
    std::vector<Timer> timers_;
    bool quit_ = false;
    std::uint32_t quit_code_ = 0, next_window_ = 0x1D600000, next_timer_ = 1, scratch_ = 0;
    GuestCall guest_call_;
    ClassResolver class_resolver_;
    std::map<std::u16string, bool> classes_asked_;  // folded "{guid}" -> found
    bool discover_class(const std::u16string& path);
    // The console's flash (system settings), formatted for a European PAL console in French.
    std::vector<std::uint8_t> flash_bytes_;
    hle::Flash flash_;
    std::uint32_t sysinfo_ = 0;  // guest copy of the SYSINFO block
    std::vector<std::uint32_t> refused_bios_;
    bool bios(sh4::Ctx& c, unsigned index);
    // DMA buffers in the emulated console RAM, seen by the process through windows.
    static constexpr std::uint32_t kDmaPoolStart = 0x0C100000, kDmaPoolEnd = 0x0D000000,
                                   kDmaWindow = 0x30000000;
    struct DmaBuffer {
        std::uint32_t process, physical;
    };
    std::map<std::uint32_t, DmaBuffer> dma_by_id_;
    std::uint32_t dma_next_ = kDmaPoolStart;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> dma_free_;            // physical, bytes
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> locked_;  // MDL -> copy
    struct Interrupt {
        std::uint32_t event;
        bool armed;  // InterruptDone since the last time it fired
        std::uint64_t fired = 0;
    };

public:
    // SYSINTR -> times delivered, for reports.
    std::map<std::uint32_t, std::uint64_t> interrupt_counts() const {
        std::map<std::uint32_t, std::uint64_t> out;
        for (const auto& [id, i] : interrupts_) out[id] = i.fired;
        return out;
    }

private:
    std::map<std::uint32_t, Interrupt> interrupts_;  // SYSINTR id -> IST event
    std::vector<std::uint32_t> unknown_interrupts_;
    std::map<std::uint32_t, std::uint32_t> enabled_;       // SYSINTR -> status bits it raises on
    std::map<std::uint32_t, std::uint32_t> acknowledged_;  // SYSINTR -> bits 0x10000 returned
    std::uint32_t enabled_of(std::uint32_t sysintr);
    std::vector<std::uint32_t> gdi_;  // device contexts and fonts (handles only)
    std::uint32_t next_gdi_ = 0x1D700000, focus_ = 0;
    bool kernel_mode_ = false;
    std::uint32_t permissions_ = 1;  // the process's own slot
    std::uint32_t send(std::uint32_t window, std::uint32_t message, std::uint32_t wparam,
                       std::uint32_t lparam);

    struct File {
        std::u16string name;
        bool device = false;
        std::unique_ptr<std::ifstream> stream;
        std::uint64_t size = 0, position = 0;
    };
    std::filesystem::path root_;
    std::map<std::uint32_t, File> files_;
    std::uint32_t next_file_ = 0x1D500000;
    std::vector<std::uint32_t> refused_device_ioctls_;
    std::vector<std::u16string> devices_opened_;  // folded names, each reported once
    std::optional<std::filesystem::path> host_path(const std::u16string& name) const;

    struct Value {
        std::u16string name;  // as written, for enumeration
        std::uint32_t type = 0;
        std::vector<std::uint8_t> data;
    };
    struct Key {
        std::u16string name;
        std::map<std::u16string, Value> values;  // by folded name
    };
    ProcessMemory& memory_;
    std::map<std::u16string, Key> keys_;            // by folded full path
    std::map<std::uint32_t, std::u16string> open_;  // handle -> folded full path
    std::uint32_t next_handle_ = 0x1D400000;
    std::vector<std::uint32_t> refused_ioctls_;
    std::vector<std::string> missing_api_sets_;

    std::optional<std::u16string> key_path(std::uint32_t handle) const;
    void ensure_key(const std::u16string& path);
};
}  // namespace dream::wince
