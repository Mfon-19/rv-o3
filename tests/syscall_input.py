#!/usr/bin/env python3
"""Exercise stdin return values, dependent consumers, and differential replay."""
from pathlib import Path
import struct
import subprocess
import tempfile

SIM = Path(__file__).resolve().parents[1] / "rvsim"


def addi(rd, rs, immediate):
    return ((immediate & 4095) << 20) | (rs << 15) | (rd << 7) | 0x13


# Read, immediately consume a0, print byte+1, read EOF, and exit with EOF+1.
program = [addi(17, 0, 5), addi(10, 0, 77), 0x73, addi(5, 10, 1),
           addi(17, 0, 1), addi(10, 5, 0), 0x73,
           addi(17, 0, 5), 0x73, addi(10, 10, 1), addi(17, 0, 93), 0x73]
with tempfile.TemporaryDirectory(prefix="rvsim-input-") as directory:
    image = Path(directory) / "input.bin"
    image.write_bytes(struct.pack("<" + "I" * len(program), *program))
    for mode in ([], ["-d"], ["-d", "-O", "width=4", "-O", "aluCount=4", "-O", "wbPorts=4"]):
        for data, expected in ((b"", b"0\n"), (b"A", b"66\n"), (b"\xff", b"256\n")):
            result = subprocess.run([str(SIM), *mode, str(image)], input=data,
                                    capture_output=True, timeout=10)
            assert result.returncode == 0, result.stderr.decode()
            assert result.stdout == expected, (mode, data, result.stdout)
            if "-d" in mode:
                assert b"final state matches" in result.stderr, result.stderr.decode()
    # An open pipe with no available byte must return immediately too.
    process = subprocess.Popen([str(SIM), "-d", str(image)], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        assert process.wait(timeout=10) == 0
        assert process.stdout.read() == b"0\n"
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        process.stdin.close()
        process.stdout.close()
        process.stderr.close()
print("syscall input: byte, EOF, empty pipe, dependent reads, and replay passed")
