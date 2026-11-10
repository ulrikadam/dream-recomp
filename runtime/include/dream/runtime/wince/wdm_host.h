#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "dream/runtime/wince/process.h"
#include "dream/runtime/wince/system_services.h"

namespace dream::wince {
// What WDEVICE.EXE does on the console, for drivers loaded into the title's own process. The
// disc's WDMLIB.DLL is the driver model (objects, IRPs, device queues) and loads the drivers
// listed under HKLM\WDMDrivers\BuiltIn (values Dll, Order, Block); WDEVICE only starts it and
// turns the file calls on "\Device\..." into WDMLIB calls. Its CreateFileW, CloseHandle,
// ReadFile, WriteFile and DeviceIoControl handlers (read from Sega Rally 2's WDEVICE.EXE) are
// reproduced here, argument checks and status mapping included.
class WdmHost final : public DeviceHost {
public:
    // Runs a guest function with any number of arguments (the fifth on go on the stack).
    using Call = std::function<std::uint32_t(std::uint32_t, const std::vector<std::uint32_t>&)>;
    // An export's address: (module, name) -> address, 0 when missing.
    using Find = std::function<std::uint32_t(const std::string&, const std::string&)>;
    WdmHost(ProcessMemory& memory, Call call, Find find);
    // The \Files object directory open devices are kept in, then WDMLIB's InitWDM, which loads
    // the registered drivers. False, with nothing done, when WDMLIB lacks an export used here.
    bool start();

    std::uint32_t open(const std::u16string& name, std::uint32_t access, std::uint32_t flags,
                       std::uint32_t handle, std::uint32_t& error) override;
    bool control(std::uint32_t object, std::uint32_t code, std::uint32_t in, std::uint32_t in_size,
                 std::uint32_t out, std::uint32_t out_size, std::uint32_t returned,
                 std::uint32_t overlapped, std::uint32_t& error) override;
    bool read_write(std::uint32_t object, bool write, std::uint32_t buffer, std::uint32_t size,
                    std::uint32_t done, std::uint32_t overlapped, std::uint32_t& error) override;
    void close(std::uint32_t object) override;

    std::uint64_t opens = 0, controls = 0, transfers = 0, failures = 0;  // for reports
    std::function<std::uint64_t()> clock;  // guest cycles, for the request trace

private:
    enum Fn {
        kInitWDM,
        kRtlInitUnicodeString,
        kObCreateDirectory,
        kOpenFileDir,  // g_OpenFileDir: a variable, the directory object
        kIoGetDeviceObjectPointer,
        kObInsertObject,
        kObRemoveObject,
        kObDereferenceObject,
        kCloseWDMDevice,
        kReadWriteWDMDevice,
        kControlWDMDevice,
        kRtlMapStatusToError,
        kCreateEventW,  // COREDLL
        kCount
    };
    ProcessMemory& memory_;
    Call call_;
    Find find_;
    std::vector<std::uint32_t> fn_;
    std::vector<std::uint32_t> failed_codes_;
    bool started_ = false;
    // A block of guest memory: a UNICODE_STRING at +0, two out-pointers at +8 and +12, the
    // string's characters from +16.
    std::uint32_t unicode(const std::u16string& text);
    bool finish(std::uint32_t status, std::uint32_t& error);
};
}  // namespace dream::wince
