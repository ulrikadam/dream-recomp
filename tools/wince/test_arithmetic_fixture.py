"""Original SH-4 PE calls to compiler arithmetic helpers, including explicit zero-divisor faults."""
import subprocess
import sys
import tempfile
from pathlib import Path

from make_sync_fixture import Module

HELPERS = ("_divls", "_divlu", "_modls", "_modlu")
PAIRS = ((7, 3), (0xFFFFFFF9, 3), (7, 0xFFFFFFFD), (0xFFFFFFF9, 0xFFFFFFFD),
         (0x80000000, 0xFFFFFFFF), (0xFFFFFFFF, 1), (0, 5), (0xFFFFFFFF, 0x80000000))


def check(executable, build_directory):
    build_directory = Path(build_directory).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="wince-arithmetic-", dir=build_directory) as scratch:
        root = Path(scratch).resolve(strict=True)
        assert root.parent == build_directory
        program = Module({"COREDLL.DLL": list(HELPERS)})
        program.code.begin()
        failure = 0
        for name in HELPERS:
            for a, b in PAIRS:
                n, d = a, b
                if name.endswith("s"):
                    n = a if a < 2**31 else a - 2**32
                    d = b if b < 2**31 else b - 2**32
                q = abs(n) // abs(d)
                if (n < 0) != (d < 0):
                    q = -q
                expected = q if name.startswith("_div") else n - q * d
                program.call(name, a, b)
                failure += 1
                program.code.expect(expected & 0xFFFFFFFF, failure)
        program.code.finish(0)
        pe = root / "arithmetic.exe"
        pe.write_bytes(program.binary())
        for base in (0x10000, 0x20000):
            result = subprocess.run([executable, "--run", str(pe), "--base", hex(base),
                                     "--steps", "10000"], capture_output=True, text=True, timeout=20)
            assert result.returncode == 0 and " r0=0\n" in result.stdout, result.stdout + result.stderr
            assert "HLE calls=32" in result.stdout, result.stdout
        for name in HELPERS:
            bad = Module({"COREDLL.DLL": [name]})
            bad.code.begin()
            bad.call(name, 7, 0)
            bad.code.finish(0)
            pe.write_bytes(bad.binary())
            result = subprocess.run([executable, "--run", str(pe), "--steps", "1000"],
                                    capture_output=True, text=True, timeout=20)
            assert result.returncode == 2, result.stdout + result.stderr
            assert "division by zero requires WinCE exception dispatch" in result.stderr
        print("SH-4 arithmetic: 32 guest assertions at two bases, all four zero-divisor faults verified.")


if __name__ == "__main__":
    check(sys.argv[1], sys.argv[2])
