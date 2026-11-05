"""Execute the original SH-4 synchronization fixture through PE import binding and DllMain."""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from make_sync_fixture import make_modules


def check(executable, build_directory):
    build_directory = Path(build_directory).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="wince-sync-", dir=build_directory) as scratch:
        root = Path(scratch).resolve(strict=True)
        assert root.parent == build_directory
        for invalid in (False, True):
            directory = root / ("invalid" if invalid else "valid")
            directory.mkdir()
            for name, content in make_modules(invalid_attach=invalid).items():
                (directory / name).write_bytes(content)
            for base in (0x10000, 0x20000):
                result = subprocess.run([executable, "--run", str(directory / "guest.exe"),
                                         "--base", hex(base), "--steps", "10000"],
                                        capture_output=True, text=True, timeout=20)
                if invalid:
                    assert result.returncode == 2, result.stdout + result.stderr
                    assert "uninitialized or deleted CE critical section" in result.stderr, result.stderr
                else:
                    assert result.returncode == 0, result.stdout + result.stderr
                    assert " r0=0\n" in result.stdout, result.stdout
                    counts = re.search(r"instructions=(\d+) HLE calls=(\d+) DLL instructions=(\d+)", result.stdout)
                    assert counts and all(int(n) > 0 for n in counts.groups()), result.stdout
                    print(f"base={hex(base)}: {counts.group(0)}; all guest assertions passed")
        print("SH-4 EXE/DLL: attach, static/dynamic exports, synchronization and failing attach verified.")


if __name__ == "__main__":
    check(sys.argv[1], sys.argv[2])
