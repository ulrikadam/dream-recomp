"""Generate a tiny SH-4 PE calling COREDLL.GetTickCount; no SDK or OS data needed."""
import argparse
import struct
from pathlib import Path


def make_pe():
    b = bytearray(0x800)
    def u16(p, v):
        struct.pack_into("<H", b, p, v)
    def u32(p, v):
        struct.pack_into("<I", b, p, v)
    u16(0, 0x5A4D)
    for p, v in {0x3C: 0x80, 0x80: 0x4550, 0xA8: 0x1000, 0xB4: 0x10000,
                 0xB8: 0x1000, 0xBC: 0x200, 0xD0: 0x2000, 0xD4: 0x200, 0xF4: 16,
                 0x100: 0x1100, 0x104: 40, 0x120: 0x1300, 0x124: 12,
                 0x180: 0x1000, 0x184: 0x1000, 0x188: 0x600, 0x18C: 0x200,
                 0x19C: 0x60000020, 0x218: 0x11190, 0x300: 0x1180, 0x30C: 0x1150,
                 0x310: 0x1190, 0x380: 0x11A0, 0x390: 0x11A0, 0x500: 0x1000, 0x504: 12}.items():
        u32(p, v)
    for p, v in {0x84: 0x1A6, 0x86: 1, 0x94: 0xE0, 0x96: 0x102, 0x98: 0x10B,
                 0xDC: 9, 0x508: 0x3018}.items():
        u16(p, v)
    for p, text in [(0x178, b".text\0"), (0x350, b"COREDLL.DLL\0"), (0x3A2, b"GetTickCount\0")]:
        b[p:p + len(text)] = text
    for i, opcode in enumerate([0x4F22, 0x7FF0, 0xD204, 0x6222, 0x420B, 0x0009, 0x7F10, 0x4F26, 0x000B, 0x0009]):
        u16(0x200 + 2 * i, opcode)
    return bytes(b)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("xb") as stream:
        stream.write(make_pe())
    print(args.output)
