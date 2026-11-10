#include "dream/runtime/wince/wdm_host.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace dream::wince {
namespace {
constexpr std::uint32_t kStatusPending = 0x103;
constexpr std::uint32_t kErrorIoPending = 997, kErrorGenFailure = 31, kErrorAccessDenied = 5,
                        kErrorInvalidParameter = 87;
constexpr std::uint32_t kFileFlagOverlapped = 0x40000000u;
// FILE_OBJECT fields WDEVICE touches: the access it was opened with, its flags and the lock
// synchronous requests take in turn (ControlWDMDevice, 0x10001510 in WDMLIB).
constexpr std::uint32_t kReadAccess = 20, kWriteAccess = 21, kFlags = 24, kLock = 52;
constexpr std::uint32_t kSynchronousIo = 2;  // FO_SYNCHRONOUS_IO
}  // namespace

WdmHost::WdmHost(ProcessMemory& memory, Call call, Find find)
    : memory_(memory), call_(std::move(call)), find_(std::move(find)) {}

bool WdmHost::start() {
    static constexpr const char* kNames[kCount] = {"InitWDM",
                                                   "RtlInitUnicodeString",
                                                   "ObCreateDirectory",
                                                   "g_OpenFileDir",
                                                   "IoGetDeviceObjectPointer",
                                                   "ObInsertObject",
                                                   "ObRemoveObject",
                                                   "ObDereferenceObject",
                                                   "CloseWDMDevice",
                                                   "ReadWriteWDMDevice",
                                                   "ControlWDMDevice",
                                                   "RtlMapStatusToError",
                                                   "CreateEventW"};
    std::vector<std::uint32_t> found(kCount);
    for (unsigned i = 0; i < kCount; ++i) {
        const auto address = find_(i == kCreateEventW ? "coredll.dll" : "wdmlib.dll", kNames[i]);
        if (!address) {
            std::fprintf(stderr, "wince: no driver host: %s not found\n", kNames[i]);
            return false;
        }
        found[i] = address;
    }
    fn_ = std::move(found);
    // WDEVICE's start: the directory its open files go in, then the driver model and drivers.
    const auto files = unicode(u"Files");
    const auto status = call_(fn_[kObCreateDirectory], {files, fn_[kOpenFileDir]});
    memory_.release(files);
    if (status) {
        std::fprintf(stderr, "wince: no driver host: ObCreateDirectory(Files) = 0x%08x\n", status);
        return false;
    }
    call_(fn_[kInitWDM], {});
    started_ = true;
    return true;
}

std::uint32_t WdmHost::unicode(const std::u16string& text) {
    const auto block = memory_.allocate(16 + 2 * static_cast<std::uint32_t>(text.size() + 1));
    if (!block)
        throw std::runtime_error("no guest memory for a driver call");
    for (std::size_t i = 0; i < text.size(); ++i)
        memory_.write16(block + 16 + 2 * static_cast<std::uint32_t>(i), text[i]);
    memory_.write16(block + 16 + 2 * static_cast<std::uint32_t>(text.size()), 0);
    memory_.write32(block + 8, 0);
    memory_.write32(block + 12, 0);
    call_(fn_[kRtlInitUnicodeString], {block, block + 16});
    return block;
}

bool WdmHost::finish(std::uint32_t status, std::uint32_t& error) {
    if (status == 0)
        return true;
    if (status != kStatusPending)  // an overlapped request still running is not a failure
        ++failures;
    error = status == kStatusPending ? kErrorIoPending : call_(fn_[kRtlMapStatusToError], {status});
    if (!error)
        error = kErrorGenFailure;
    return false;
}

std::uint32_t WdmHost::open(const std::u16string& name, std::uint32_t access, std::uint32_t flags,
                            std::uint32_t handle, std::uint32_t& error) {
    if (!started_) {
        error = kErrorGenFailure;
        return 0;
    }
    const auto block = unicode(name);
    const auto status =
        call_(fn_[kIoGetDeviceObjectPointer], {block, access, block + 8, block + 12});
    const auto object = memory_.read32(block + 8);
    memory_.release(block);
    if (!finish(status, error))
        return 0;
    // Synchronous use: WDMLIB serialises the file's requests on this event, a lock taken by
    // waiting on it and released by setting it, so it starts signalled and resets itself.
    if (!(flags & kFileFlagOverlapped)) {
        memory_.write32(object + kFlags, memory_.read32(object + kFlags) | kSynchronousIo);
        memory_.write32(object + kLock, call_(fn_[kCreateEventW], {0, 0, 1, 0}));
    }
    call_(fn_[kObInsertObject], {object, handle, memory_.read32(fn_[kOpenFileDir])});
    ++opens;
    return object;
}

bool WdmHost::control(std::uint32_t object, std::uint32_t code, std::uint32_t in,
                      std::uint32_t in_size, std::uint32_t out, std::uint32_t out_size,
                      std::uint32_t returned, std::uint32_t overlapped, std::uint32_t& error) {
    if (!returned || (!in && in_size) || (!out && out_size)) {
        error = kErrorInvalidParameter;
        return false;
    }
    // The code's access bits (14: read, 15: write) against how the device was opened.
    if (((code & 0x4000u) && !memory_.read8(object + kReadAccess)) ||
        ((code & 0x8000u) && !memory_.read8(object + kWriteAccess))) {
        error = kErrorAccessDenied;
        return false;
    }
    ++controls;
    // DREAM_WINCE_TRACE_DRIVER=N: the first N requests, their input and status (bring-up aid).
    static int traced = [] {
        const char* v = std::getenv("DREAM_WINCE_TRACE_DRIVER");
        return v ? std::atoi(v) : 0;
    }();
    std::string input;
    if (traced > 0)
        for (std::uint32_t i = 0; i + 4 <= in_size && i < 24; i += 4) {
            char word[12];
            std::snprintf(word, sizeof word, " %08x", memory_.read32(in + i));
            input += word;
        }
    const auto status = call_(fn_[kControlWDMDevice],
                              {object, code, in, in_size, out, out_size, returned, overlapped});
    if (traced > 0) {
        --traced;
        std::fprintf(stderr, "driver: @%llu 0x%08x in%s -> 0x%08x%s\n",
                     static_cast<unsigned long long>(clock ? clock() / 200000 : 0), code,
                     input.c_str(), status, overlapped ? " (overlapped)" : "");
    }
    if (status && status != kStatusPending &&
        std::find(failed_codes_.begin(), failed_codes_.end(), code) == failed_codes_.end()) {
        failed_codes_.push_back(code);  // each code reported once
        std::fprintf(stderr, "wince: driver request 0x%08x (in %u, out %u bytes): status 0x%08x\n",
                     code, in_size, out_size, status);
    }
    return finish(status, error);
}

bool WdmHost::read_write(std::uint32_t object, bool write, std::uint32_t buffer, std::uint32_t size,
                         std::uint32_t done, std::uint32_t overlapped, std::uint32_t& error) {
    if (!done) {
        error = kErrorInvalidParameter;
        return false;
    }
    if (!memory_.read8(object + (write ? kWriteAccess : kReadAccess))) {
        error = kErrorAccessDenied;
        return false;
    }
    if (!size) {
        memory_.write32(done, 0);
        return true;
    }
    if (!buffer) {
        error = kErrorInvalidParameter;
        return false;
    }
    ++transfers;
    // IRP_MJ_READ 2, IRP_MJ_WRITE 3
    return finish(
        call_(fn_[kReadWriteWDMDevice], {object, write ? 3u : 2u, buffer, size, done, overlapped}),
        error);
}

void WdmHost::close(std::uint32_t object) {
    call_(fn_[kObRemoveObject], {object});
    call_(fn_[kCloseWDMDevice], {object});
}
}  // namespace dream::wince
