#!/usr/bin/env python3
"""Interface tests: stdin input, the display pipes, and self-modifying code.

Each builds a small binary, runs rvsim on it (mostly under -d, so the
reference model must agree), and checks what comes out.
"""
import os
from pathlib import Path
import struct
import subprocess
import tempfile

SIM = str(Path(__file__).resolve().parents[1] / "rvsim")
WIDE = ["-O", "width=8", "-O", "wbPorts=8", "-O", "aluCount=8"]


def addi(rd, rs, immediate):
    return ((immediate & 4095) << 20) | (rs << 15) | (rd << 7) | 0x13


def jal_ra(pc, target):
    offset = target - pc
    return (((offset >> 20) & 1) << 31 | ((offset >> 1) & 1023) << 21 |
            ((offset >> 11) & 1) << 20 | ((offset >> 12) & 255) << 12 |
            1 << 7 | 0x6f)


def words(*values):
    return struct.pack(f"<{len(values)}I", *values)


def check_diff(result):
    assert result.returncode == 0, result.stderr.decode()
    assert b"final state matches" in result.stderr, result.stderr.decode()


def stdin_input(program):
    """Syscall 5: a byte or EOF, consumed immediately, identical under -d."""
    # Read, consume a0, print byte+1, read EOF, and exit with EOF+1
    program.write_bytes(words(
        addi(17, 0, 5), addi(10, 0, 77), 0x73, addi(5, 10, 1),
        addi(17, 0, 1), addi(10, 5, 0), 0x73,
        addi(17, 0, 5), 0x73, addi(10, 10, 1), addi(17, 0, 93), 0x73))
    for mode in ([], ["-d"], ["-d", "-O", "width=4", "-O", "aluCount=4", "-O", "wbPorts=4"]):
        for data, expected in ((b"", b"0\n"), (b"A", b"66\n"), (b"\xff", b"256\n")):
            result = subprocess.run([SIM, *mode, str(program)], input=data,
                                    capture_output=True, timeout=10)
            assert result.stdout == expected, (mode, data, result.stdout)
            if mode:
                check_diff(result)
            else:
                assert result.returncode == 0, result.stderr.decode()
    # An open pipe with no byte available must not block either
    process = subprocess.Popen([SIM, "-d", str(program)], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    assert process.stdin and process.stdout
    try:
        assert process.wait(timeout=10) == 0
        assert process.stdout.read() == b"0\n"
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        process.stdin.close()
        process.stdout.close()


PALETTE = 768                         # indexed frames: 256 XRGB words here
XRGB = (512, 2, 2, 1)                 # {pixels, width, height, format}
INDEXED = (512, 2, 2, 2, PALETTE)     # format 2 also names its palette
XRGB_FRAME = (0x112233, 0xff0000, 0x00ff00, 0x0000ff)
INDEXED_FRAME = tuple(i * 0x010203 for i in (0x33, 0x22, 0x11, 0))


def frame_image(descriptor=XRGB):
    """Present one 2x2 frame, then read and print three key events."""
    # The first store dirties a cached pixel word before presenting, so a
    # frame read from stale backing memory would show the old value. An
    # indexed frame sees that word as four indices: 0x33 0x22 0x11 0
    code = [addi(5, 0, 512), 0x00112337, addi(6, 6, 0x233), 0x0062a023,
            addi(10, 0, 256), addi(17, 0, 6), 0x73]
    for _ in range(3):
        code += [addi(17, 0, 7), 0x73, addi(17, 0, 4), 0x73]
    code += [addi(10, 0, 0), addi(17, 0, 93), 0x73]
    data = bytearray(PALETTE + 1024)
    struct.pack_into(f"<{len(code)}I", data, 0, *code)
    struct.pack_into(f"<{len(descriptor)}I", data, 256, *descriptor)
    struct.pack_into("<4I", data, 512, 0, 0xff0000, 0x00ff00, 0x0000ff)
    struct.pack_into("<256I", data, PALETTE, *(i * 0x010203 for i in range(256)))
    return data


def run_with_pipes(program, flags, keys=b""):
    """Run with a frame file and a key pipe preloaded with keys; return
    the result and the frame bytes written"""
    read_fd, write_fd = os.pipe()
    os.write(write_fd, keys)
    os.close(write_fd)
    try:
        with tempfile.TemporaryFile() as frames:
            result = subprocess.run(
                [SIM, *flags, "--frame-fd", str(frames.fileno()),
                 "--key-fd", str(read_fd), str(program)],
                pass_fds=(frames.fileno(), read_fd), capture_output=True, timeout=10)
            frames.seek(0)
            return result, frames.read()
    finally:
        os.close(read_fd)


def display(program):
    """Syscalls 6 and 7: frames read through the caches in both pixel
    formats, key events replayed under -d, malformed frames rejected"""
    for descriptor, pixels in ((XRGB, XRGB_FRAME), (INDEXED, INDEXED_FRAME)):
        program.write_bytes(frame_image(descriptor))
        for mode in ([], ["-d"], ["-d", "-O", "flatMemory=1"], ["-d", *WIDE]):
            # 'w' pressed and released, then no more events
            result, frame = run_with_pipes(program, mode, struct.pack("<II", 0x177, 0x77))
            assert result.returncode == 0, result.stderr.decode()
            assert result.stdout == b"00000177\n00000077\nffffffff\n", result.stdout
            assert frame == struct.pack("<7I", 0x31465652, 2, 2, *pixels), \
                "missing, duplicate, or stale frame"
            if mode:
                check_diff(result)
    # Zero or oversized dimensions, an unknown format, pixels or a palette
    # outside memory
    for descriptor in ((512, 0, 2, 1), (512, 641, 2, 1), (512, 2, 481, 1),
                       (512, 2, 2, 9), (0xfffffff0, 2, 2, 1),
                       (512, 2, 2, 2, 0xfffffc00)):
        program.write_bytes(frame_image(descriptor))
        result = subprocess.run([SIM, "-d", str(program)], capture_output=True, timeout=10)
        assert result.returncode == 1 and b"display:" in result.stderr, result.stderr
    program.write_bytes(frame_image())
    result = subprocess.run([SIM, str(program)], capture_output=True, timeout=10)
    assert result.returncode == 1 and b"requires --frame-fd" in result.stderr
    # An empty, open key pipe must never block retirement
    program.write_bytes(words(addi(17, 0, 7), 0x73, addi(10, 10, 1), addi(17, 0, 93), 0x73))
    read_fd, write_fd = os.pipe()
    try:
        with tempfile.TemporaryFile() as frames:
            result = subprocess.run(
                [SIM, "-d", "--frame-fd", str(frames.fileno()), "--key-fd", str(read_fd),
                 str(program)], pass_fds=(frames.fileno(), read_fd),
                capture_output=True, timeout=10)
            assert result.returncode == 0, result.stderr.decode()
    finally:
        os.close(read_fd)
        os.close(write_fd)


def self_modifying(program):
    """A program that rewrites one of its own instructions runs the new one."""
    # Call a function, overwrite its ADDI with a different ADDI, call it
    # again. Flat memory makes the new instruction visible without I-cache
    # coherence; the FENCE drains the store before the second call is fetched
    code = [
        0x10000293,       # addi t0, zero, 256 (function address)
        0x00250337,       # lui t1, 0x250
        0x51330313,       # addi t1, t1, 0x513 (encodes addi a0,a0,2)
        0x00000513,       # addi a0, zero, 0
        jal_ra(16, 256),  # a0 = 1
        0x0062a023,       # sw t1, 0(t0)
        0x0000000f,       # fence
        jal_ra(28, 256),  # a0 = 1 + 2
        0x00100893,       # addi a7, zero, 1 (print)
        0x00000073,
        0x00000513,       # addi a0, zero, 0
        0x05d00893,       # addi a7, zero, 93 (exit)
        0x00000073,
    ]
    code += [0x00000013] * (64 - len(code))
    code += [0x00150513, 0x00008067]  # 256: addi a0,a0,1; ret
    program.write_bytes(words(*code))
    for width in (1, 2, 8):
        result = subprocess.run(
            [SIM, "-d", "-O", "flatMemory=1", "-O", f"width={width}",
             "-O", "fetchQSize=16", str(program)], capture_output=True, timeout=10)
        check_diff(result)
        assert result.stdout == b"3\n", result.stdout


with tempfile.TemporaryDirectory(prefix="rvsim-interface-") as directory:
    image = Path(directory) / "program.bin"
    stdin_input(image)
    display(image)
    self_modifying(image)
print("interface: stdin input, display frames and keys, self-modifying code passed")
