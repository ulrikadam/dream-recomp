#include "dream/runtime/wince/process.h"

#include "doctest.h"

using namespace dream::wince;
namespace {
struct Guest {
    ProcessMemory memory;
    Kernel kernel;
    dream::sh4::Ctx cpu{};
    std::uint32_t data = memory.allocate(4096);
    std::uint32_t call(const char* name, std::uint32_t a = 0, std::uint32_t b = 0,
                       std::uint32_t c = 0, std::uint32_t d = 0) {
        cpu.r[4] = a;
        cpu.r[5] = b;
        cpu.r[6] = c;
        cpu.r[7] = d;
        const auto trap = kernel.resolve(Import{"COREDLL.dll", name});
        REQUIRE(trap.has_value());
        const auto called = kernel.invoke(cpu, memory, *trap);
        REQUIRE(called);
        return cpu.r[0];
    }
    void wide(std::uint32_t at, std::u16string_view text) {
        for (unsigned i = 0; i < text.size(); ++i) memory.write16(at + i * 2, text[i]);
        memory.write16(at + static_cast<unsigned>(text.size()) * 2, 0);
    }
};
constexpr std::uint32_t failed = 0xFFFFFFFFu;
}  // namespace

TEST_CASE("WinCE synchronization: EventModify, pulses, timeout and last-error") {
    Guest g;
    const auto e = g.call("CreateEventW", 0, 0, 0);
    REQUIRE(e != 0);
    g.call("SetLastError", 42);
    CHECK(g.call("EventModify", e, 3) == 1);
    CHECK(g.kernel.last_error() == 42);
    CHECK(g.call("WaitForSingleObject", e, failed) == 0);
    CHECK(g.call("WaitForSingleObject", e, 25) == 258);
    CHECK(g.cpu.cycles == 25u * 200000u);
    CHECK(g.call("SetEvent", e) == 1);
    CHECK(g.call("EventModify", e, 4) == 0);
    CHECK(g.kernel.last_error() == 87);
    CHECK(g.call("WaitForSingleObject", e) == 0);  // Invalid operation did not reset it.
    CHECK(g.call("EventModify", e, 3) == 1);
    CHECK(g.call("EventModify", e, 2) == 1);
    CHECK(g.call("WaitForSingleObject", e) == 258);
    CHECK(g.call("EventModify", e, 3) == 1);
    CHECK(g.call("EventModify", e, 1) == 1);
    CHECK(g.call("WaitForSingleObject", e) == 258);  // No waiter to receive the pulse.
    CHECK(g.call("SetEvent", e) == 1);
    CHECK(g.call("PulseEvent", e) == 1);
    CHECK(g.call("WaitForSingleObject", e) == 258);
    const auto before = g.cpu.cycles;
    CHECK_THROWS_WITH(g.call("WaitForSingleObject", e, failed),
                      "unsatisfied infinite wait needs a WinCE thread scheduler");
    CHECK(g.cpu.cycles == before);
    CHECK(g.call("WaitForSingleObject", e, 0xFFFFFFFEu) == 258);
    CHECK(g.cpu.cycles - before == 0x7FFFFFFFull * 200000u);
    CHECK(g.call("CloseHandle", e) == 1);
    CHECK(g.call("EventModify", e, 3) == 0);
    CHECK(g.kernel.last_error() == 6);
}

TEST_CASE("WinCE synchronization: recursive mutex ownership survives duplicate handles") {
    Guest g;
    g.wide(g.data, u"mutex");
    const auto h = g.call("CreateMutex", 0, 1, g.data);
    REQUIRE(h != 0);
    const auto alias = g.call("CreateMutexW", 0, 1, g.data);
    REQUIRE(alias != h);
    CHECK(g.kernel.last_error() == 183);
    CHECK(g.call("WaitForSingleObject", alias, failed) == 0);
    CHECK(g.call("WaitForSingleObject", h, failed) == 0);
    CHECK(g.call("CloseHandle", h) == 1);
    CHECK(g.call("ReleaseMutex", h) == 0);
    CHECK(g.kernel.last_error() == 6);
    for (unsigned i = 0; i < 3; ++i) CHECK(g.call("ReleaseMutex", alias) == 1);
    CHECK(g.call("ReleaseMutex", alias) == 0);
    CHECK(g.kernel.last_error() == 288);
    const auto reopened = g.call("CreateMutexW", 0, 1, g.data);
    CHECK(g.kernel.last_error() == 183);
    CHECK(g.call("ReleaseMutex", reopened) == 0);  // Initial ownership ignored for existing name.
    CHECK(g.call("WaitForSingleObject", reopened, 10) == 0);
    CHECK(g.cpu.cycles == 0);
    CHECK(g.call("ReleaseMutex", alias) == 1);
    CHECK(g.call("SetEvent", alias) == 0);
    CHECK(g.kernel.last_error() == 6);
    CHECK(g.call("CloseHandle", alias) == 1);
    CHECK(g.call("CloseHandle", reopened) == 1);
    const auto fresh = g.call("CreateMutexW", 0, 1, g.data);
    CHECK(g.kernel.last_error() == 0);
    CHECK(g.call("ReleaseMutex", fresh) == 1);
    const auto event = g.call("CreateEvent", 0, 0, 0);
    CHECK(g.call("ReleaseMutex", event) == 0);
    CHECK(g.kernel.last_error() == 6);
}

TEST_CASE("WinCE synchronization: shared case-sensitive Unicode names and handle lifetime") {
    Guest g;
    g.wide(g.data, u"M\u00e9dia");
    g.wide(g.data + 64, u"m\u00e9dia");
    const auto event = g.call("CreateEventW", 0, 1, 1, g.data);
    CHECK(g.call("CreateMutexW", 0, 0, g.data) == 0);
    CHECK(g.kernel.last_error() == 6);
    const auto mutex = g.call("CreateMutexW", 0, 0, g.data + 64);
    REQUIRE(mutex != 0);
    CHECK(g.call("CreateEventW", 0, 0, 0, g.data + 64) == 0);
    CHECK(g.kernel.last_error() == 6);
    const auto alias = g.call("CreateEventW", 0, 0, 0, g.data);
    CHECK(g.kernel.last_error() == 183);
    CHECK(g.call("CloseHandle", event) == 1);
    CHECK(g.call("WaitForSingleObject", alias) == 0);
    CHECK(g.call("WaitForSingleObject", alias) == 0);  // Keeps original manual-reset attributes.
    CHECK(g.call("CloseHandle", alias) == 1);
    CHECK(g.call("CreateMutexW", 0, 0, g.data) != 0);  // Last close released the shared name.
    CHECK(g.kernel.last_error() == 0);
    CHECK(g.call("CloseHandle", mutex) == 1);
}

TEST_CASE("WinCE synchronization: invalid names and attributes create no object") {
    Guest g;
    CHECK(g.call("CreateMutexW", 1, 0, 0) == 0);
    CHECK(g.kernel.last_error() == 120);
    g.wide(g.data, u"dir\\name");
    CHECK(g.call("CreateEventW", 0, 0, 0, g.data) == 0);
    CHECK(g.kernel.last_error() == 123);
    CHECK(g.call("CreateMutexW", 0, 0, g.data + 1) == 0);
    CHECK(g.kernel.last_error() == 998);
    CHECK(g.call("CreateMutexW", 0, 0, 0xFFFFFFFEu) == 0);
    CHECK(g.kernel.last_error() == 998);
    for (unsigned i = 0; i < 260; ++i) g.memory.write16(g.data + i * 2, u'a');
    CHECK(g.call("CreateEventW", 0, 0, 0, g.data) == 0);
    CHECK(g.kernel.last_error() == 123);
    g.memory.write16(g.data + 258 * 2, 0);
    CHECK(g.call("CreateMutexW", 0, 0, g.data) == 0x1D000000);
}

TEST_CASE("WinCE synchronization: wait-any order, consumption and complete validation") {
    Guest g;
    const auto e0 = g.call("CreateEventW", 0, 0, 1);
    const auto e1 = g.call("CreateEventW", 0, 0, 1);
    const auto mutex = g.call("CreateMutexW", 0, 0, 0);
    g.memory.write32(g.data, e0);
    g.memory.write32(g.data + 4, e1);
    g.memory.write32(g.data + 8, mutex);
    CHECK(g.call("WaitForMultipleObjects", 3, g.data, 0, failed) == 0);
    CHECK(g.call("WaitForMultipleObjects", 3, g.data, 0, failed) == 1);
    CHECK(g.call("WaitForMultipleObjects", 3, g.data, 0, failed) == 2);
    CHECK(g.call("ReleaseMutex", mutex) == 1);
    CHECK(g.call("ReleaseMutex", mutex) == 0);  // Other waits did not acquire the mutex.
    CHECK(g.call("WaitForMultipleObjects", 2, g.data, 0, 7) == 258);
    CHECK(g.cpu.cycles == 7u * 200000u);
    g.call("SetEvent", e0);
    SUBCASE("invalid later handle") {
        g.memory.write32(g.data + 4, 0);
        CHECK(g.call("WaitForMultipleObjects", 2, g.data, 0, 7) == failed);
        CHECK(g.kernel.last_error() == 6);
    }
    SUBCASE("duplicate handle") {
        g.memory.write32(g.data + 4, e0);
        CHECK(g.call("WaitForMultipleObjects", 2, g.data, 0, 7) == failed);
        CHECK(g.kernel.last_error() == 87);
    }
    SUBCASE("wait-all is not supported by Windows CE") {
        CHECK(g.call("WaitForMultipleObjects", 2, g.data, 1, 7) == failed);
        CHECK(g.kernel.last_error() == 87);
    }
    CHECK(g.call("WaitForSingleObject", e0) == 0);  // Failure did not consume first event.
    CHECK(g.cpu.cycles == 7u * 200000u);
}

TEST_CASE("WinCE synchronization: wait array bounds and timeouts") {
    Guest g;
    for (unsigned i = 0; i < 64; ++i)
        g.memory.write32(g.data + i * 4, g.call("CreateEventW", 0, 0, i == 63));
    CHECK(g.call("WaitForMultipleObjects", 64, g.data, 0, 0) == 63);
    CHECK(g.call("WaitForMultipleObjects", 65, g.data, 0, 0) == failed);
    CHECK(g.kernel.last_error() == 87);
    CHECK(g.call("WaitForMultipleObjects", 0, g.data, 0, 0) == failed);
    CHECK(g.kernel.last_error() == 87);
    for (const auto pointer : {0u, g.data + 1, g.data + 4092, 0xFFFFFFFCu}) {
        CHECK(g.call("WaitForMultipleObjects", 2, pointer, 0, 0) == failed);
        CHECK(g.kernel.last_error() == 998);
    }
    CHECK(g.call("WaitForMultipleObjects", 64, g.data, 0, 0x80000000u) == 258);
    CHECK(g.cpu.cycles == 0x7FFFFFFFull * 200000u);
    CHECK_THROWS_AS(g.call("WaitForMultipleObjects", 64, g.data, 0, failed), std::runtime_error);
}

TEST_CASE("WinCE synchronization: legacy critical section footprint, recursion and deletion") {
    Guest g;
    const auto p = g.data + 32;
    g.memory.write32(p - 4, 0xBADCAFE);
    g.memory.write32(p + 16, 0xCAFE1234);
    g.call("InitializeCriticalSection", p);
    CHECK(g.memory.read32(p - 4) == 0xBADCAFE);
    CHECK(g.memory.read32(p + 16) == 0xCAFE1234);
    CHECK(g.memory.read32(p) == 0);
    CHECK(g.memory.read32(p + 4) == 0);
    CHECK(g.memory.read32(p + 12) == 0);
    const auto token = g.memory.read32(p + 8);
    REQUIRE(token != 0);
    CHECK(g.call("CloseHandle", token) == 0);
    CHECK(g.kernel.last_error() == 6);
    g.call("EnterCriticalSection", p);
    g.call("EnterCriticalSection", p);
    CHECK(g.call("TryEnterCriticalSection", p) == 1);
    CHECK(g.memory.read32(p) == 3);
    CHECK(g.memory.read32(p + 4) == g.call("GetCurrentThreadId"));
    CHECK_THROWS_AS(g.call("DeleteCriticalSection", p), std::runtime_error);
    CHECK(g.memory.read32(p) == 3);
    for (unsigned i = 0; i < 3; ++i) g.call("LeaveCriticalSection", p);
    CHECK(g.memory.read32(p + 4) == 0);
    CHECK_THROWS_AS(g.call("LeaveCriticalSection", p), std::runtime_error);
    g.call("DeleteCriticalSection", p);
    CHECK(g.memory.read64(p) == 0);
    CHECK(g.memory.read64(p + 8) == 0);
    CHECK_THROWS_AS(g.call("TryEnterCriticalSection", p), std::runtime_error);
    g.call("InitializeCriticalSection", p);
    CHECK(g.memory.read32(p + 8) != token);
    CHECK(g.call("TryEnterCriticalSection", p) == 1);
    g.call("LeaveCriticalSection", p);
    g.call("DeleteCriticalSection", p);
}

TEST_CASE("WinCE synchronization: critical section misuse cannot silently succeed") {
    Guest g;
    CHECK_THROWS_AS(g.call("InitializeCriticalSection", 0), std::runtime_error);
    CHECK_THROWS_AS(g.call("InitializeCriticalSection", g.data + 1), std::runtime_error);
    CHECK_THROWS_AS(g.call("EnterCriticalSection", g.data), std::runtime_error);
    g.call("InitializeCriticalSection", g.data + 16);
    for (const auto delta : {4u, 16u, 20u, 28u})
        CHECK_THROWS_AS(g.call("InitializeCriticalSection", g.data + delta), std::runtime_error);
    g.memory.write_bytes(g.data + 64, g.memory.read_bytes(g.data + 16, 16));
    CHECK_THROWS_AS(g.call("EnterCriticalSection", g.data + 64), std::runtime_error);
    g.memory.write32(g.data + 28, 1);
    CHECK_THROWS_AS(g.call("EnterCriticalSection", g.data + 16), std::runtime_error);
    g.memory.write32(g.data + 28, 0);
    g.call("EnterCriticalSection", g.data + 16);
    CHECK(g.memory.read32(g.data + 16) == 1);
    g.call("LeaveCriticalSection", g.data + 16);
    g.call("DeleteCriticalSection", g.data + 16);
}

TEST_CASE("WinCE synchronization: failed cross-page critical section writes are atomic") {
    Guest g;
    const auto base = g.memory.virtual_alloc(0, 8192, 0x3000, 4);
    const auto p = base + 4096 - 8;
    g.memory.write_bytes(p, std::array<std::uint8_t, 16>{});
    g.memory.virtual_protect(base + 4096, 4096, 2, g.data);
    CHECK_THROWS_AS(g.call("InitializeCriticalSection", p), std::runtime_error);
    CHECK(g.memory.read64(p) == 0);
    g.memory.virtual_protect(base + 4096, 4096, 4, g.data);
    g.call("InitializeCriticalSection", p);  // No partial host registration from failed init.
    g.call("EnterCriticalSection", p);
    const auto state = g.memory.read_bytes(p, 16);
    g.memory.virtual_protect(base + 4096, 4096, 2, g.data);
    CHECK_THROWS_AS(g.call("LeaveCriticalSection", p), std::runtime_error);
    CHECK_THROWS_AS(g.call("EnterCriticalSection", p), std::runtime_error);
    CHECK(g.memory.read_bytes(p, 16) == state);
    g.memory.virtual_protect(base + 4096, 4096, 4, g.data);
    g.call("LeaveCriticalSection", p);
    g.call("DeleteCriticalSection", p);
}

TEST_CASE("WinCE synchronization: bounded shared handles and critical section resources") {
    Guest g;
    std::vector<std::uint32_t> handles;
    for (unsigned i = 0; i < 1024; ++i) {
        const auto h = i % 2 ? g.call("CreateEventW") : g.call("CreateMutexW");
        REQUIRE(h != 0);
        handles.push_back(h);
    }
    CHECK(g.call("CreateMutexW") == 0);
    CHECK(g.kernel.last_error() == 8);
    CHECK(g.call("CreateEventW") == 0);
    CHECK(g.kernel.last_error() == 8);
    CHECK(g.call("CloseHandle", handles[0]) == 1);
    const auto fresh = g.call("CreateEventW");
    REQUIRE(fresh != 0);
    CHECK(fresh != handles[0]);
    CHECK(g.call("WaitForSingleObject", handles[0]) == failed);
    const auto p = g.memory.allocate(4097 * 16);
    for (unsigned i = 0; i < 4096; ++i) g.call("InitializeCriticalSection", p + i * 16);
    CHECK_THROWS_AS(g.call("InitializeCriticalSection", p + 4096 * 16), std::runtime_error);
    g.call("DeleteCriticalSection", p);
    g.call("InitializeCriticalSection", p + 4096 * 16);
    CHECK(g.call("TryEnterCriticalSection", p + 4096 * 16) == 1);
}
