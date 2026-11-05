"""Synthetic fixtures only: no Sega or Windows CE binaries in the repository."""

import json
import struct
from dataclasses import dataclass

import pytest

from dcdisc.wince import (
    IMAGE_FILE_MACHINE_SH4,
    PEFormatError,
    inspect_wince_filesystem,
    parse_pe,
    report_json,
    report_text,
)


def make_pe(machine=IMAGE_FILE_MACHINE_SH4, imported=b"COREDLL.DLL\x00"):
    """Tiny synthetic PE32 with one .rdata section and one import descriptor."""
    out = bytearray(0x600)
    out[:2] = b"MZ"
    struct.pack_into("<I", out, 0x3C, 0x80)
    out[0x80:0x84] = b"PE\x00\x00"
    # machine, sections, timestamp, symbols, symbol count, opt-header size, characteristics
    struct.pack_into("<HHIIIHH", out, 0x84, machine, 1, 0, 0, 0, 0xE0, 0x2102)
    opt = 0x98
    struct.pack_into("<H", out, opt, 0x10B)
    struct.pack_into("<I", out, opt + 16, 0x1010)  # entry RVA
    struct.pack_into("<I", out, opt + 28, 0x8C010000)  # guest VA
    struct.pack_into("<I", out, opt + 56, 0x2000)  # image size
    struct.pack_into("<I", out, opt + 60, 0x200)  # headers
    struct.pack_into("<H", out, opt + 68, 9)  # Windows CE GUI subsystem
    struct.pack_into("<I", out, opt + 92, 16)  # data directories
    struct.pack_into("<II", out, opt + 104, 0x1100, 40)  # import descriptor RVA, size
    sec = 0x178
    out[sec:sec + 8] = b".rdata\x00\x00"
    struct.pack_into("<IIII", out, sec + 8, 0x400, 0x1000, 0x400, 0x200)
    struct.pack_into("<IIIII", out, 0x300, 0x1180, 0, 0, 0x1150, 0x1180)
    out[0x350:0x350 + len(imported)] = imported
    return bytes(out)


def test_pe32_sh4_import_and_sections():
    m = parse_pe(make_pe(), "GAME.EXE")
    assert m is not None and m.is_sh4
    assert m.machine == IMAGE_FILE_MACHINE_SH4
    assert m.is_dll
    assert m.subsystem == 9
    assert m.entry_rva == 0x1010 and m.entry_va == 0x8C011010
    assert m.imports == ["COREDLL.DLL"]
    assert m.sections[0].name == ".rdata"
    assert m.sections[0].raw_offset == 0x200


def test_non_pe_and_other_processor():
    assert parse_pe(b"this is data") is None
    x = parse_pe(make_pe(machine=0x14C))
    assert x is not None and not x.is_sh4
    assert x.machine == 0x14C


def test_bad_headers_fail_cleanly():
    with pytest.raises(PEFormatError):
        parse_pe(b"MZ")
    data = bytearray(make_pe())
    struct.pack_into("<I", data, 0x3C, 0xFFFFFFF0)
    with pytest.raises(PEFormatError):
        parse_pe(bytes(data))
    data = bytearray(make_pe())
    struct.pack_into("<H", data, 0x84 + 2, 0xFFFF)
    with pytest.raises(PEFormatError):
        parse_pe(bytes(data))


@dataclass
class FakeIP:
    title: str = "SYNTHETIC WINCE"
    product_number: str = "TEST-000"
    boot_filename: str = "1ST_READ.BIN"
    is_wince: bool = True


@dataclass
class FakeEntry:
    name: str
    path: str
    size: int
    is_dir: bool = False


class FakeFs:
    def __init__(self, data):
        self.data = data

    def files(self):
        return [FakeEntry(path.rsplit("/", 1)[-1], path, len(content))
                for path, content in self.data.items()]

    def read(self, e):
        return self.data[e.path]


def test_disc_inventory_wince_detected_from_bit_and_kernel():
    files = {
        "/0WINCEOS.BIN": b"proprietary marker, not parsed",
        "/GAME.EXE": make_pe(),
        "/DATA/TEXTURE.PVR": b"not a PE",
        "/1ST_READ.BIN": b"Dreamcast boot code",
    }
    r = inspect_wince_filesystem(FakeIP(), FakeFs(files))
    assert r.detected
    assert len(r.kernel_files) == 1
    assert len(r.modules) == 1
    assert r.modules[0].path == "GAME.EXE"
    assert r.unrecognized_candidates == ["1ST_READ.BIN"]
    assert not r.runtime_supported
    obj = json.loads(report_json(r))
    assert obj["modules"][0]["imports"] == ["COREDLL.DLL"]
    assert "commercial games unsupported" in report_text(r)


def test_wince_cli_inventory(gdi_path, capsys):
    from dcdisc.cli import main
    assert main(["wince-report", gdi_path, "--json"]) == 0
    report = json.loads(capsys.readouterr().out)
    assert report["title"] == "SYNTHETIC TEST DISC"
    assert not report["runtime_supported"]
    assert main(["wince-report", gdi_path]) == 0
    assert "commercial games unsupported" in capsys.readouterr().out


def test_disc_inventory_only_pe_can_signal_wince():
    r = inspect_wince_filesystem(
        FakeIP(is_wince=False), FakeFs({"/GAME.EXE": make_pe()})
    )
    assert r.detected
    assert r.indicators == ["SH-4 PE32 module with Windows CE subsystem in ISO9660"]


def test_disc_inventory_not_wince():
    r = inspect_wince_filesystem(
        FakeIP(is_wince=False), FakeFs({"/1ST_READ.BIN": b"ordinary game"})
    )
    assert not r.detected
    assert not r.modules


def test_oversized_candidates_are_not_loaded():
    class Fs:
        def files(self):
            return [FakeEntry("HUGE.EXE", "/HUGE.EXE", 100_000_000)]
        def read(self, e):
            raise AssertionError("Oversized data must never be read")

    r = inspect_wince_filesystem(FakeIP(), Fs())
    assert r.skipped_candidates and "HUGE.EXE" in r.skipped_candidates[0]
