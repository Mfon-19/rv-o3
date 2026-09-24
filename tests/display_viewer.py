#!/usr/bin/env python3
"""Exercise native rendering with SDL's offscreen driver and fragmented pipes."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
VIEWER = ROOT / "doom/build/doom-viewer"
ENV = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_RENDER_DRIVER="software")


def check(packet, expected_status, capture=None, fragmented=False):
    command = [str(VIEWER)] + (["--capture", str(capture)] if capture else [])
    process = subprocess.Popen(command, env=ENV, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        if fragmented:
            for byte in packet[:16]:
                process.stdin.write(bytes([byte]))
                process.stdin.flush()
                time.sleep(0.002)
            process.stdin.write(packet[16:])
        else:
            process.stdin.write(packet)
        process.stdin.close()
        process.stdin = None
        out, err = process.communicate(timeout=10)
        assert process.returncode == expected_status, err.decode()
        assert out == b"", out
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


with tempfile.TemporaryDirectory(prefix="rvsim-viewer-") as directory:
    bmp = Path(directory) / "frame.bmp"
    packet = struct.pack("<7I", 0x31465652, 2, 2, 0xff0000, 0x00ff00, 0x0000ff, 0xffffff)
    check(packet, 0, bmp, fragmented=True)
    data = bmp.read_bytes()
    assert data[:2] == b"BM"
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bits = struct.unpack_from("<H", data, 28)[0]
    assert bits in (24, 32), bits
    stride = ((width * bits + 31) // 32) * 4
    def pixel(x, y):
        row = height - 1 - y if height > 0 else y
        p = offset + row * stride + x * (bits // 8)
        return tuple(data[p:p + 3])
    w, h = width, abs(height)
    assert pixel(w // 4, h // 4) == (0, 0, 255)
    assert pixel(3 * w // 4, h // 4) == (0, 255, 0)
    assert pixel(w // 4, 3 * h // 4) == (255, 0, 0)
    assert pixel(3 * w // 4, 3 * h // 4) == (255, 255, 255)
    check(packet[:7], 1)
    check(packet[:-1], 1)
    check(struct.pack("<III", 0x31465652, 0xffffffff, 200), 1)
    # Recreate the texture when dimensions change between complete frames.
    check(packet + struct.pack("<4I", 0x31465652, 1, 1, 0x112233), 0)
print("viewer: fragmented frames, RGB channels, resizing, malformed input, and EOF passed")
