"""Build an original SH-4 PE exercising guest DirectDraw/3D/Sound COM (no SDK needed).

Run with dream-wince --run output.exe --steps 100000 --capture new-directory.
The guest draws a Gouraud triangle and loops a generated PCM tone. The runner captures
the final frame and one second of audio; this is an offline ABI test, not a game.
"""
import argparse
import math
import struct
from pathlib import Path


BASE = 0x10000


class Address(int):
    """Guest address requiring a PE HIGHLOW relocation."""


class Indirect(int):
    """Load an argument through a guest pointer slot."""


class Assembler:
    def __init__(self):
        self.code = bytearray()
        self.pending = []
        self.relocations = []

    def emit(self, op):
        self.code += struct.pack("<H", op)

    def load(self, reg, value):
        indirect = isinstance(value, Indirect)
        self.pending.append((len(self.code), reg, Address(value) if indirect else value))
        self.emit(0)
        if indirect:
            self.emit(0x6002 | (reg << 8) | (reg << 4))

    def flush(self):
        branch = len(self.code)
        self.emit(0)
        self.emit(0x0009)
        if len(self.code) % 4:
            self.emit(0x0009)
        for at, reg, value in self.pending:
            literal = len(self.code)
            disp = (literal - ((at + 4) & ~3)) // 4
            assert 0 <= disp <= 255
            struct.pack_into("<H", self.code, at, 0xD000 | (reg << 8) | disp)
            self.code += struct.pack("<I", value)
            if isinstance(value, Address):
                self.relocations.append(0x1000 + literal)
        self.pending.clear()
        delta = (len(self.code) - branch - 4) // 2
        assert 0 <= delta <= 2047
        struct.pack_into("<H", self.code, branch, 0xA000 | delta)

    def call(self, args, iat=None, slot=None):
        for i, value in enumerate(args):
            if i < 4:
                self.load(4 + i, value)
            else:
                self.load(0, value)
                self.emit(0x1F00 | i)  # after the four register home slots at SP+0..12
        if iat is not None:
            self.load(2, Indirect(iat))
        else:
            self.emit(0x6242)  # vtable = *this
            self.load(3, slot * 4)
            self.emit(0x323C)  # add r3,r2
            self.emit(0x6222)  # method = vtable[slot]
        self.flush()
        self.emit(0x420B)  # jsr @r2
        self.emit(0x0009)
        self.emit(0x2008)  # tst r0,r0
        self.emit(0x8903)  # bt success (preserve failing HRESULT and return otherwise)
        self.emit(0x7F20)
        self.emit(0x4F26)
        self.emit(0x000B)
        self.emit(0x0009)


def make_pe():
    b = bytearray(0x9200)

    def u16(p, v):
        struct.pack_into("<H", b, p, v)

    def u32(p, v):
        struct.pack_into("<I", b, p, v)

    def raw(rva):
        return rva - 0x1000 + 0x200

    def string(rva, value):
        value = value.encode("ascii") + b"\0"
        b[raw(rva):raw(rva) + len(value)] = value

    def words(rva, values):
        for i, value in enumerate(values):
            u32(raw(rva) + i * 4, value)

    def ptr(rva):
        return Address(BASE + rva)

    a = Assembler()
    a.emit(0x4F22)  # save PR
    a.emit(0x7FE0)  # 16 bytes register home space plus four stack arguments
    draw, surface, d3d, device, sound, buffer = (Indirect(BASE + 0x5000 + i * 4) for i in range(6))
    a.call([0, Address(draw), ptr(0x5100), 0], iat=BASE + 0x2180)
    a.call([draw, 320, 240, 32, 0, 0], slot=21)
    a.call([draw, ptr(0x5200), Address(surface), 0], slot=6)
    a.call([surface, 0, 0, 0, 0x1000400, ptr(0x5300)], slot=5)
    a.call([draw, ptr(0x5110), Address(d3d)], slot=0)
    a.call([d3d, ptr(0x5120), surface, Address(device), 0], slot=8)
    a.call([device], slot=9)
    a.call([device, 4, 0x1C4, ptr(0x5400), 3, 0], slot=28)
    a.call([device], slot=10)
    a.call([surface, 0, ptr(0x5500), 0x10, 0], slot=25)
    a.call([surface, 0], slot=32)
    a.call([0, Address(sound), 0], iat=BASE + 0x2188)
    a.call([sound, ptr(0x5600), Address(buffer), 0], slot=3)
    a.call([buffer, 0, 8820, ptr(0x5020), ptr(0x5024), 0, 0, 2], slot=11)
    a.load(1, ptr(0x6000)); a.load(2, Indirect(BASE + 0x5020)); a.load(3, 4410); a.flush()
    loop = len(a.code)
    for op in [0x6015, 0x2201, 0x7202, 0x4310]:
        a.emit(op)
    a.emit(0x8B00 | (((loop - len(a.code) - 4) // 2) & 255))
    a.call([buffer, Indirect(BASE + 0x5020), 8820, 0, 0], slot=19)
    a.call([buffer, 0, 0, 1], slot=12)
    for op in [0xE000, 0x7F20, 0x4F26, 0x000B, 0x0009]:
        a.emit(op)
    assert len(a.code) <= 0x1000
    b[0x200:0x200 + len(a.code)] = a.code

    words(0x5100, [0x9C59509A, 0x11D139BD, 0xC0004A8C, 0xC530D94F])
    words(0x5110, [0xBB223240, 0x11D0E72B, 0xAA00B4A9, 0x3E99C000])
    words(0x5120, [0x84E63DE0, 0x11CF46AA, 0x00006F81, 0x6E1520C0])
    words(0x5200, [124, 1]); words(0x5268, [0x2200])  # DDSURFACEDESC2
    words(0x5300, [100]); words(0x5350, [0x142033])  # DDBLTFX fill
    for i, (x, y, color) in enumerate([(160, 45, 0xFFFF6040), (285, 215, 0xFF40FF80), (35, 215, 0xFF4080FF)]):
        struct.pack_into("<ffffIIff", b, raw(0x5400) + i * 32, x, y, 0.5, 1, color, 0, 0, 0)
    words(0x5500, [124])
    words(0x5600, [20, 0x80, 8820, 0, BASE + 0x5640]); a.relocations.append(0x5610)
    struct.pack_into("<HHIIHHH", b, raw(0x5640), 1, 1, 44100, 88200, 2, 16, 0)
    for i in range(4410):
        struct.pack_into("<h", b, raw(0x6000) + i * 2, round(math.sin(2 * math.pi * 440 * i / 44100) * 6000))

    # Import descriptors, lookup tables and writable IATs.
    words(0x2000, [0x2140, 0, 0, 0x2100, 0x2180])
    words(0x2014, [0x2148, 0, 0, 0x2110, 0x2188])
    string(0x2100, "DDRAW.DLL"); string(0x2110, "DSOUND.DLL")
    for rva in [0x2140, 0x2180]: words(rva, [0x2200, 0])
    for rva in [0x2148, 0x2188]: words(rva, [0x2240, 0])
    string(0x2202, "DirectDrawCreateEx"); string(0x2242, "DirectSoundCreate")
    reloc = bytearray()
    for page in sorted({r & ~0xFFF for r in a.relocations}):
        entries = [0x3000 | (r & 0xFFF) for r in a.relocations if r & ~0xFFF == page]
        if len(entries) % 2: entries.append(0)
        reloc += struct.pack("<II", page, 8 + len(entries) * 2)
        reloc += struct.pack("<" + "H" * len(entries), *entries)
    b[raw(0x3000):raw(0x3000) + len(reloc)] = reloc
    u16(0, 0x5A4D)
    for p, v in {0x3C: 0x80, 0x80: 0x4550, 0xA8: 0x1000, 0xB4: BASE,
                 0xB8: 0x1000, 0xBC: 0x200, 0xD0: 0xA000, 0xD4: 0x200,
                 0xF4: 16, 0x100: 0x2000, 0x104: 60, 0x120: 0x3000, 0x124: len(reloc),
                 0x180: 0x9000, 0x184: 0x1000, 0x188: 0x9000, 0x18C: 0x200,
                 0x19C: 0xE0000060}.items(): u32(p, v)
    for p, v in {0x84: 0x1A6, 0x86: 1, 0x94: 0xE0, 0x96: 0x102, 0x98: 0x10B, 0xDC: 9}.items(): u16(p, v)
    b[0x178:0x17D] = b".test"
    return bytes(b)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("xb") as stream:
        stream.write(make_pe())
    print(args.output)
