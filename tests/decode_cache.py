#!/usr/bin/env python3
"""Changed instruction bits must invalidate host decode memoization."""
from pathlib import Path
import struct
import subprocess
import tempfile


def jal(pc, target):
    offset = target - pc
    return (((offset >> 20) & 1) << 31 | ((offset >> 1) & 1023) << 21 |
            ((offset >> 11) & 1) << 20 | ((offset >> 12) & 255) << 12 |
            1 << 7 | 0x6f)  # jal ra, target


# Call a function, replace its ADDI with a different ADDI, then call again.
# Flat memory makes the changed instruction visible without I-cache coherence.
# A serializing FENCE drains the store before the second call is fetched.
words = [
    0x10000293,           # addi t0, zero, 256 (function address)
    0x00250337,           # lui t1, 0x250
    0x51330313,           # addi t1, t1, 0x513 (encoding of addi a0,a0,2)
    0x00000513,           # addi a0, zero, 0
    jal(16, 256),
    0x0062a023,           # sw t1, 0(t0)
    0x0000000f,           # fence
    jal(28, 256),
    0x00100893,           # addi a7, zero, 1 (print)
    0x00000073,           # ecall: expected a0 = 1 + 2
    0x00000513,           # addi a0, zero, 0
    0x05d00893,           # addi a7, zero, 93 (exit)
    0x00000073,
]
words += [0x00000013] * (64 - len(words))
words += [0x00150513, 0x00008067]  # addi a0,a0,1; ret

with tempfile.TemporaryDirectory(prefix="rvsim-decode-") as directory:
    image = Path(directory) / "changed-code.bin"
    image.write_bytes(struct.pack(f"<{len(words)}I", *words))
    for width in (1, 2, 8):
        result = subprocess.run(
            ["./rvsim", "-d", "-O", "flatMemory=1", "-O", f"width={width}",
             "-O", "fetchQSize=16", str(image)], capture_output=True, timeout=10,
        )
        assert result.returncode == 0, result.stderr.decode()
        assert result.stdout == b"3\n", result.stdout
print("decode cache: changed instruction bits verified at widths 1, 2, 8")
