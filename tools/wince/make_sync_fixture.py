"""Original SH-4 EXE/DLL testing CE synchronization during DLL_PROCESS_ATTACH and exports.

No SDK, game data or Microsoft binaries are used. The generated pair is executable with
dream-wince --run guest.exe. Relocating the EXE also changes the DLL's placement.
"""
import argparse
import struct
from pathlib import Path

from make_directx_fixture import Address, Assembler, Indirect

BASE = 0x10000


class Code(Assembler):
    def begin(self):
        rva = 0x1000 + len(self.code)
        self.emit(0x4F22)  # save caller PR
        self.emit(0x7FF0)  # CE's four register home slots
        return rva

    def invoke(self, iat, *args):
        assert len(args) <= 4
        for i, value in enumerate(args):
            self.load(4 + i, value)
        self.load(2, Indirect(iat))
        self.flush()
        self.emit(0x420B)
        self.emit(0x0009)

    def finish(self, result=None):
        if result is not None:
            self.load(0, result)
            self.flush()
        for op in (0x7F10, 0x4F26, 0x000B, 0x0009):
            self.emit(op)

    def expect(self, expected, failure):
        self.load(1, expected)
        self.flush()
        self.emit(0x3010)  # cmp/eq r1,r0
        branch = len(self.code)
        self.emit(0)
        self.finish(failure)
        delta = (len(self.code) - branch - 4) // 2
        assert 0 <= delta < 128
        struct.pack_into("<H", self.code, branch, 0x8900 | delta)

    def save(self, address):
        self.load(1, Address(address))
        self.flush()
        self.emit(0x2102)  # mov.l r0,@r1


class Module:
    def __init__(self, imports, dll=False):
        self.b = bytearray(0x5200)
        self.code = Code()
        self.iat = {}
        self.exports = []
        self.dll = dll
        cursor = 0x2100
        for group, (library, symbols) in enumerate(imports.items()):
            library_rva = cursor
            self.text(cursor, library)
            cursor = (cursor + len(library) + 4) & ~3
            lookup = cursor
            table = lookup + (len(symbols) + 1) * 4
            cursor = table + (len(symbols) + 1) * 4
            self.words(0x2000 + group * 20, [lookup, 0, 0, library_rva, table])
            for index, symbol in enumerate(symbols):
                self.words(lookup + index * 4, [cursor])
                self.words(table + index * 4, [cursor])
                self.text(cursor + 2, symbol)
                self.iat[symbol] = BASE + table + index * 4
                cursor = (cursor + len(symbol) + 6) & ~3
        assert cursor < 0x3000
        self.import_size = (len(imports) + 1) * 20

    @staticmethod
    def raw(rva):
        return rva - 0x1000 + 0x200

    def words(self, rva, values):
        for i, value in enumerate(values):
            struct.pack_into("<I", self.b, self.raw(rva) + i * 4, value)

    def text(self, rva, value, wide=False):
        data = (value + "\0").encode("utf-16le" if wide else "ascii")
        at = self.raw(rva)
        self.b[at:at + len(data)] = data

    def call(self, name, *args):
        self.code.invoke(self.iat[name], *args)

    def binary(self):
        a = self.code
        assert not a.pending and len(a.code) <= 0x1000
        self.b[0x200:0x200 + len(a.code)] = a.code
        if self.exports:
            self.words(0x4000 + 16, [1, len(self.exports), len(self.exports), 0x4040, 0x4080, 0x40C0])
            for i, (_, rva) in enumerate(self.exports):
                self.words(0x4040 + i * 4, [rva])
            # PE name pointers are sorted; ordinals still index the original address table.
            for i, (ordinal, (name, _)) in enumerate(sorted(enumerate(self.exports), key=lambda item: item[1][0])):
                self.words(0x4080 + i * 4, [0x4100 + i * 32])
                struct.pack_into("<H", self.b, self.raw(0x40C0) + i * 2, ordinal)
                self.text(0x4100 + i * 32, name)
        reloc = bytearray()
        for page in sorted({r & ~0xFFF for r in a.relocations}):
            entries = [0x3000 | (r & 0xFFF) for r in a.relocations if r & ~0xFFF == page]
            if len(entries) % 2:
                entries.append(0)
            reloc += struct.pack("<II", page, 8 + len(entries) * 2)
            reloc += struct.pack("<" + "H" * len(entries), *entries)
        assert len(reloc) <= 0x1000
        at = self.raw(0x3000)
        self.b[at:at + len(reloc)] = reloc
        for p, v in {0x3C: 0x80, 0x80: 0x4550, 0xA8: 0x1000, 0xB4: BASE,
                     0xB8: 0x1000, 0xBC: 0x200, 0xD0: 0x6000, 0xD4: 0x200,
                     0xF4: 16, 0x100: 0x2000, 0x104: self.import_size,
                     0x120: 0x3000, 0x124: len(reloc), 0x180: 0x5000,
                     0x184: 0x1000, 0x188: 0x5000, 0x18C: 0x200,
                     0x19C: 0xE0000060}.items():
            struct.pack_into("<I", self.b, p, v)
        if self.exports:
            struct.pack_into("<II", self.b, 0xF8, 0x4000, 0x200)
        for p, v in {0: 0x5A4D, 0x84: 0x1A6, 0x86: 1, 0x94: 0xE0,
                     0x96: 0x2102 if self.dll else 0x102, 0x98: 0x10B, 0xDC: 9}.items():
            struct.pack_into("<H", self.b, p, v)
        self.b[0x178:0x180] = b".fixture"
        return bytes(self.b)


def make_modules(invalid_attach=False):
    dll = Module({"COREDLL.DLL": [
        "InitializeCriticalSection", "EnterCriticalSection", "LeaveCriticalSection",
        "TryEnterCriticalSection", "DeleteCriticalSection", "CreateEventW", "SetEvent",
        "CreateMutexW", "WaitForSingleObject", "ReleaseMutex", "CloseHandle"]}, dll=True)
    d = dll.code
    event, mutex, critical, count = 0x15000, 0x15004, Address(0x15020), 0x15040
    dll.text(0x5100, "fixture-ready", True)
    dll.text(0x5140, "fixture-mutex", True)
    d.begin()  # DllMain: check hInstance and reason, then initialize shared state.
    d.emit(0x6043)
    d.expect(Address(BASE), 0)
    d.emit(0x6053)
    d.expect(1, 0)
    dll.call("EnterCriticalSection" if invalid_attach else "InitializeCriticalSection", critical)
    dll.call("CreateEventW", 0, 0, 0, Address(0x15100))
    d.save(event)
    dll.call("CreateMutexW", 0, 0, Address(0x15140))
    d.save(mutex)
    d.finish(1)
    dll.exports.append(("Work", d.begin()))
    dll.call("EnterCriticalSection", critical)
    dll.call("EnterCriticalSection", critical)
    dll.call("TryEnterCriticalSection", critical)
    d.expect(1, 0xE1)
    for _ in range(2):
        dll.call("WaitForSingleObject", Indirect(mutex), 0xFFFFFFFF)
        d.expect(0, 0xE2)
    for _ in range(2):
        dll.call("ReleaseMutex", Indirect(mutex))
        d.expect(1, 0xE3)
    dll.call("SetEvent", Indirect(event))
    d.expect(1, 0xE4)
    for _ in range(3):
        dll.call("LeaveCriticalSection", critical)
    d.load(0, Indirect(count))
    d.flush()
    d.emit(0x7001)
    d.save(count)
    d.finish()
    dll.exports.append(("Finish", d.begin()))
    dll.call("DeleteCriticalSection", critical)
    for handle in (event, mutex):
        dll.call("CloseHandle", Indirect(handle))
        d.expect(1, 0xE5)
    d.finish(0)

    exe = Module({"sync.dll": ["Work"], "COREDLL.DLL": [
        "LoadLibraryW", "GetModuleHandleW", "GetProcAddressW", "CreateEventW", "CreateMutexW",
        "GetLastError", "WaitForSingleObject", "WaitForMultipleObjects", "ReleaseMutex",
        "EventModify", "CloseHandle"]})
    a = exe.code
    library, proc, event, mutex = 0x15000, 0x15004, 0x15008, 0x1500C
    for rva, text in ((0x5100, "sync.dll"), (0x5140, "Work"), (0x5180, "fixture-ready"),
                      (0x51C0, "fixture-mutex")):
        exe.text(rva, text, True)
    a.begin()
    exe.call("Work")
    a.expect(1, 1)
    exe.call("LoadLibraryW", Address(0x15100))
    a.save(library)
    exe.call("GetModuleHandleW", Address(0x15100))
    a.expect(Indirect(library), 2)
    exe.call("GetProcAddressW", Indirect(library), Address(0x15140))
    a.save(proc)
    a.invoke(proc)
    a.expect(2, 3)  # DLL_PROCESS_ATTACH did not run again on LoadLibrary.
    exe.call("GetProcAddressW", Indirect(library), 1)
    a.expect(Indirect(proc), 4)
    exe.call("CreateEventW", 0, 1, 0, Address(0x15180))
    a.save(event)
    exe.call("GetLastError")
    a.expect(183, 5)
    exe.call("WaitForSingleObject", Indirect(event), 0)
    a.expect(0, 6)
    exe.call("WaitForSingleObject", Indirect(event), 0)
    a.expect(258, 7)  # Existing event retained DLL's auto-reset attributes.
    exe.call("CreateMutexW", 0, 1, Address(0x151C0))
    a.save(mutex)
    exe.call("ReleaseMutex", Indirect(mutex))
    a.expect(0, 8)  # Opening an existing mutex did not grant initial ownership.
    exe.call("GetLastError")
    a.expect(288, 9)
    exe.call("EventModify", Indirect(event), 3)
    a.expect(1, 10)
    exe.call("WaitForMultipleObjects", 2, Address(event), 0, 0xFFFFFFFF)
    a.expect(0, 11)
    exe.call("WaitForMultipleObjects", 2, Address(event), 0, 0xFFFFFFFF)
    a.expect(1, 12)
    exe.call("ReleaseMutex", Indirect(mutex))
    a.expect(1, 13)
    exe.call("GetProcAddressW", Indirect(library), 2)
    a.save(proc)
    a.invoke(proc)
    a.expect(0, 14)
    exe.call("WaitForSingleObject", Indirect(event), 0)
    a.expect(258, 15)  # DLL closing its handle did not close the EXE's alias.
    for handle in (event, mutex):
        exe.call("CloseHandle", Indirect(handle))
        a.expect(1, 16)
    a.finish(0)
    return {"guest.exe": exe.binary(), "sync.dll": dll.binary()}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="new directory for the original EXE/DLL pair")
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=False)
    for name, content in make_modules().items():
        (args.directory / name).write_bytes(content)
    print(args.directory / "guest.exe")
