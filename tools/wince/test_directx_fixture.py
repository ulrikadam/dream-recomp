"""End-to-end test of an original guest PE, including relocations and captured pixels/PCM."""
import math
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

from make_directx_fixture import make_pe


def check(executable, build_directory):
    # All temporary files live in a fresh child of the CMake build directory.
    build_directory = Path(build_directory).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="wince-directx-", dir=build_directory) as scratch:
        root = Path(scratch).resolve(strict=True)
        assert root.parent == build_directory
        pe = root / "guest.exe"
        pe.write_bytes(make_pe())
        previous = None
        for base in (0x10000, 0x20000):
            capture = root / str(base)
            result = subprocess.run([executable, "--run", str(pe), "--base", hex(base),
                                     "--steps", "100000", "--capture", str(capture)],
                                    capture_output=True, text=True, timeout=20, check=True)
            assert " r0=0\n" in result.stdout, result.stdout + result.stderr
            assert "HLE calls=16" in result.stdout, result.stdout
            image = (capture / "guest.ppm").read_bytes()
            prefix = b"P6\n320 240\n255\n"
            assert image.startswith(prefix) and len(image) == len(prefix) + 320 * 240 * 3
            pixels = image[len(prefix):]
            assert pixels[:3] == bytes((0x14, 0x20, 0x33))
            center = (120 * 320 + 160) * 3
            assert all(40 <= c <= 255 for c in pixels[center:center + 3])
            assert pixels[center:center + 3] != pixels[:3]
            with wave.open(str(capture / "guest.wav"), "rb") as wav:
                assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getnframes()) == (2, 2, 44100, 44100)
                pcm = wav.readframes(44100)
            samples = struct.unpack("<88200h", pcm)
            for i in range(44100):
                expected = round(math.sin(2 * math.pi * 440 * (i % 4410) / 44100) * 6000)
                assert samples[2 * i] == samples[2 * i + 1] == expected
            if previous is not None:
                assert previous == (image, pcm), "Relocation changed the rendered output"
            previous = (image, pcm)
        print("Guest DirectX PE: factory/vtable calls, pixels, PCM and relocated execution verified.")


if __name__ == "__main__":
    check(sys.argv[1], sys.argv[2])
