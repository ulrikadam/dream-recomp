"""Read-only reconnaissance of Windows CE Dreamcast discs.

This module deliberately does not attempt to execute Windows CE or translate its binaries.
It identifies candidate PE modules and their imports for a later SH-4/WinCE runtime.
No proprietary game or Microsoft data is stored in a report.

The existing dcdisc IP.BIN and ISO9660 readers are used without changing the
Katana recompilation pipeline.
"""

from __future__ import annotations

import struct
from dataclasses import asdict, dataclass, field
from pathlib import PurePosixPath
from typing import Optional

IMAGE_FILE_MACHINE_SH4 = 0x01A6
IMAGE_SUBSYSTEM_WINDOWS_CE_GUI = 9
# Deliberately bounded: scanning a malformed disc should not allocate a huge buffer.
MAX_MODULE_BYTES = 32 * 1024 * 1024
MAX_SECTIONS = 96
MAX_IMPORT_DESCRIPTORS = 512
MAX_DLL_NAME_LENGTH = 260
WINCE_KERNEL_FILES = frozenset({"0WINCEOS.BIN", "WINCEOS.BIN"})
PE_FILE_EXTENSIONS = frozenset({".EXE", ".DLL", ".CPL", ".OCX", ".DRV"})


class PEFormatError(ValueError):
    """File has a DOS/PE signature but its headers are malformed or incomplete."""


@dataclass
class PESection:
    name: str
    virtual_address: int
    virtual_size: int
    raw_offset: int
    raw_size: int


@dataclass
class PEModule:
    path: str
    size: int
    machine: int
    is_sh4: bool
    is_dll: bool
    image_base: int
    entry_rva: int
    entry_va: int
    subsystem: int
    sections: list[PESection]
    imports: list[str]
    warnings: list[str] = field(default_factory=list)


@dataclass
class WinCEReport:
    title: str
    product_number: str
    detected: bool
    indicators: list[str]
    kernel_files: list[dict]
    modules: list[PEModule]
    unrecognized_candidates: list[str]
    skipped_candidates: list[str]
    notes: list[str]
    # This is intentional: detection != recompilation or WinCE API support.
    runtime_supported: bool = False


def _u16(data: bytes, at: int) -> int:
    if at < 0 or at + 2 > len(data):
        raise PEFormatError("truncated PE header")
    return struct.unpack_from("<H", data, at)[0]


def _u32(data: bytes, at: int) -> int:
    if at < 0 or at + 4 > len(data):
        raise PEFormatError("truncated PE header")
    return struct.unpack_from("<I", data, at)[0]


def _cstring(data: bytes, offset: Optional[int]) -> Optional[str]:
    if offset is None or offset < 0 or offset >= len(data):
        return None
    end = data.find(b"\0", offset, min(len(data), offset + MAX_DLL_NAME_LENGTH + 1))
    if end < 0 or end == offset:
        return None
    raw = data[offset:end]
    if any(c < 32 or c > 126 for c in raw):
        return None
    return raw.decode("ascii", "strict")


def parse_pe(data: bytes, path: str = "<memory>") -> Optional[PEModule]:
    """Parse PE32 metadata; return None for non-PE data.

    Only module metadata is decoded. Relocations, imports by ordinal, WinCE ROM
    modules, and CE-specific E32 formats are *not* loaded or executed here.
    """
    if len(data) < 2 or data[:2] != b"MZ":
        return None
    if len(data) < 0x40:
        raise PEFormatError("DOS header is too short")
    pe_offset = _u32(data, 0x3C)
    if pe_offset < 0x40 or pe_offset + 24 > len(data):
        raise PEFormatError("invalid or truncated PE offset")
    if data[pe_offset:pe_offset + 4] != b"PE\0\0":
        return None  # MZ executable, but not a valid PE image

    machine = _u16(data, pe_offset + 4)
    count = _u16(data, pe_offset + 6)
    opt_size = _u16(data, pe_offset + 20)
    characteristics = _u16(data, pe_offset + 22)
    opt = pe_offset + 24
    end_opt = opt + opt_size
    if end_opt > len(data) or opt_size < 96:
        raise PEFormatError("truncated optional PE header")
    if count > MAX_SECTIONS:
        raise PEFormatError("unreasonable section count")

    magic = _u16(data, opt)
    if magic != 0x10B:
        raise PEFormatError("unsupported PE optional-header format (expected PE32)")

    entry_rva = _u32(data, opt + 16)
    image_base = _u32(data, opt + 28)
    subsystem = _u16(data, opt + 68)
    size_headers = _u32(data, opt + 60)

    section_table = end_opt
    if section_table + count * 40 > len(data):
        raise PEFormatError("truncated PE section table")
    sections: list[PESection] = []
    for index in range(count):
        p = section_table + index * 40
        name = data[p:p + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        sections.append(PESection(
            name=name,
            virtual_address=_u32(data, p + 12),
            virtual_size=_u32(data, p + 8),
            raw_offset=_u32(data, p + 20),
            raw_size=_u32(data, p + 16),
        ))

    def file_offset(rva: int) -> Optional[int]:
        if 0 <= rva < min(size_headers, len(data)):
            return rva
        for s in sections:
            span = max(s.raw_size, s.virtual_size)
            if s.virtual_address <= rva < s.virtual_address + span:
                delta = rva - s.virtual_address
                target = s.raw_offset + delta
                if delta < s.raw_size and target < len(data):
                    return target
        return None

    warnings: list[str] = []
    imports: set[str] = set()
    directory_count = _u32(data, opt + 92)
    if directory_count >= 2 and opt_size >= 96 + 16:
        imports_rva = _u32(data, opt + 96 + 8)
        imports_size = _u32(data, opt + 96 + 12)
        descriptor_start = file_offset(imports_rva) if imports_rva else None
        if imports_rva and descriptor_start is None:
            warnings.append("import table could not be mapped to file bytes")
        if descriptor_start is not None:
            count_to_read = min(MAX_IMPORT_DESCRIPTORS, (imports_size + 19) // 20)
            for n in range(count_to_read):
                p = descriptor_start + n * 20
                if p + 20 > len(data):
                    warnings.append("truncated import table")
                    break
                if data[p:p + 20] == bytes(20):
                    break
                name_rva = _u32(data, p + 12)
                name = _cstring(data, file_offset(name_rva))
                if name:
                    imports.add(name)
                else:
                    warnings.append(f"unreadable import name at descriptor {n}")

    return PEModule(
        path=path,
        size=len(data),
        machine=machine,
        is_sh4=machine == IMAGE_FILE_MACHINE_SH4,
        is_dll=bool(characteristics & 0x2000),
        image_base=image_base,
        entry_rva=entry_rva,
        entry_va=(image_base + entry_rva) & 0xFFFFFFFF,
        subsystem=subsystem,
        sections=sections,
        imports=sorted(imports),
        warnings=warnings,
    )


def inspect_wince_filesystem(ip, fs) -> WinCEReport:
    """Survey an already opened ISO9660 filesystem, using an existing IpBin object.

    Requires only ``ip.is_wince`` and ``fs.files()/fs.read(entry)``.
    Keeping the inspection independent makes it testable with synthetic images.
    """
    entries = sorted(fs.files(), key=lambda e: e.path.upper())
    kernel_entries = [e for e in entries if e.name.upper() in WINCE_KERNEL_FILES]
    indicators = []
    if ip.is_wince:
        indicators.append("IP.BIN Windows CE peripheral bit")
    if kernel_entries:
        indicators.append("Windows CE kernel filename in ISO9660")
    kernel_files = [dict(path=e.path.lstrip("/"), size=e.size) for e in kernel_entries]
    modules: list[PEModule] = []
    unrecognized: list[str] = []
    skipped: list[str] = []

    # Read only likely PE candidates. A proprietary 0WINCEOS.BIN can be large and is not
    # interpreted as a PE file by this reconnaissance step.
    for entry in entries:
        if entry.is_dir:
            continue
        name = entry.name.upper()
        if name in WINCE_KERNEL_FILES:
            continue
        ext = PurePosixPath(name).suffix
        is_boot_bin = name == ip.boot_filename.upper() and ext == ".BIN"
        if ext not in PE_FILE_EXTENSIONS and not is_boot_bin:
            continue
        path = entry.path.lstrip("/")
        if entry.size > MAX_MODULE_BYTES:
            skipped.append(f"{path} (size exceeds {MAX_MODULE_BYTES} bytes)")
            continue
        try:
            data = fs.read(entry)
            mod = parse_pe(data, path=path)
            if mod is None:
                unrecognized.append(path)
            else:
                modules.append(mod)
        except (OSError, ValueError) as error:
            skipped.append(f"{path} ({error})")

    if any(mod.is_sh4 and mod.subsystem == IMAGE_SUBSYSTEM_WINDOWS_CE_GUI
           for mod in modules) and not indicators:
        indicators.append("SH-4 PE32 module with Windows CE subsystem in ISO9660")

    notes = [
        "Read-only metadata discovery: does not execute or recompile Windows CE games.",
        "A WinCE kernel/ROM image can contain modules not exposed as ISO9660 files.",
        "Only PE32 candidates with ordinary DOS/PE headers are enumerated.",
        "Do not commit proprietary OS files, game executables or extracted game assets.",
    ]
    return WinCEReport(
        title=ip.title,
        product_number=ip.product_number,
        detected=bool(indicators),
        indicators=indicators,
        kernel_files=kernel_files,
        modules=modules,
        unrecognized_candidates=unrecognized,
        skipped_candidates=skipped,
        notes=notes,
    )


def inspect_wince_image(image) -> WinCEReport:
    """Open the existing Dreamcast IP.BIN and ISO9660 readers on a disc image."""
    from .ipbin import parse_ipbin, read_ipbin
    from .iso9660 import Iso9660

    return inspect_wince_filesystem(parse_ipbin(read_ipbin(image)), Iso9660(image))


def report_json(report: WinCEReport) -> str:
    import json
    return json.dumps(asdict(report), ensure_ascii=False, indent=2)


def report_text(report: WinCEReport) -> str:
    lines = [
        f"Title:      {report.title}",
        f"Product:    {report.product_number}",
        f"Windows CE: {'detected' if report.detected else 'not detected'}",
        f"Indicators: {', '.join(report.indicators) if report.indicators else '-'}",
        f"Kernel:     {', '.join(x['path'] for x in report.kernel_files) if report.kernel_files else '-'}",
        f"PE modules: {len(report.modules)}",
    ]
    for module in report.modules:
        arch = "SH-4" if module.is_sh4 else f"machine 0x{module.machine:04X}"
        lines.extend([
            f"  {module.path}: {arch}, entry VA 0x{module.entry_va:08X}, "
            f"subsystem {module.subsystem}, {len(module.sections)} sections",
            f"    imports: {', '.join(module.imports) if module.imports else '-'}",
        ])
    if report.unrecognized_candidates:
        lines.append("Not PE: " + ", ".join(report.unrecognized_candidates))
    for item in report.skipped_candidates:
        lines.append("Skipped: " + item)
    lines.append("Runtime support: experimental components only; commercial games unsupported")
    return "\n".join(lines) + "\n"
