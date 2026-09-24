#!/usr/bin/env python3
"""Architectural bandwidth, latency, and precise target-fault regressions."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SIM = str(ROOT / "rvsim")
WIDE = ["-C", str(ROOT / "configs/doom.cfg")]


def run(flags, image=None, expected=0):
    args = [SIM, *flags]
    if image is not None:
        args += ["-c", "100000", str(image)]
    result = subprocess.run(args, capture_output=True, timeout=15)
    assert result.returncode == expected, (args, result.stderr.decode())
    return result


def cycles(result):
    return int(re.search(rb"rvsim: (\d+) cycles", result.stderr)[1])


with tempfile.TemporaryDirectory(prefix="rvsim-architecture-") as directory:
    image = Path(directory) / "program.hex"

    def write(words):
        image.write_text("\n".join(f"{word:08x}" for word in words) + "\n")

    for knob in ("aguCount=0", "aguCount=9", "dataPorts=0", "dataPorts=9",
                 "fetchBytes=4", "fetchBytes=12", "fetchBytes=128"):
        run(["-p", "-O", knob], expected=1)
    run(["-p", "-O", "fetchBytes=64", "-O", "fetchQSize=8"], expected=1)
    run([*WIDE, "-p", "-O", "l1i.line=32", "-O", "l1d.line=32",
         "-O", "l2.line=32"], expected=1)
    for size in (8, 16, 32, 64):
        run([*WIDE, "-p", "-O", f"fetchBytes={size}"])

    exit_zero = [0x00000513, 0x05d00893, 0x00000073]
    for config in ([], WIDE):
        for latency in (1, 7):
            flags = [*config, "-O", "flatMemory=1", "-O", f"flatLatency={latency}"]
            run([*flags, "-d"], ROOT / "tests/wrong_path_target.hex")
            # Real JALR, JAL, and taken conditional targets fault. An
            # untaken conditional with a misaligned target must not fault.
            for words in ([0x00200093, 0x00008067], [0x0020006f], [0x00000163]):
                write(words)
                result = run(flags, image, expected=1)
                assert b"misaligned branch target" in result.stderr
            write([0x00001163, *exit_zero])  # bne x0,x0,+2 (not taken)
            run([*flags, "-d"], image)

    # A slow older branch must also squash a younger JALR to an address
    # outside memory before fetch reaches it; a real one faults cleanly.
    wrong_path_range = [0x00100093, 0xfff00137, 0x0210c1b3,  # x1=1, x2=0xfff00000, x3=x1/x1
                        0x00118a63, 0x00010067,              # beq x3,x1,+20; jalr x0,0(x2)
                        0x00000013, 0x00000013, 0x00000013, *exit_zero]
    for config in ([], WIDE):
        for memory in (["-O", "flatMemory=1"], ["-O", "flatMemory=1", "-O", "flatLatency=7"], []):
            flags = [*config, *memory]
            write(wrong_path_range)
            run([*flags, "-d"], image)
            write([0xfff00137, 0x00010067])  # lui x2,0xfff00; jalr x0,0(x2)
            result = run(flags, image, expected=1)
            assert b"instruction fetch at 0xfff00000 is outside memory" in result.stderr

    def measure(words, agus, ports, latency=1, flat=False):
        write(words)
        return cycles(run([*WIDE, "-d", "-O", f"flatMemory={int(flat)}",
                           "-O", f"flatLatency={latency}", "-O", f"l1d.latency={latency}",
                           "-O", "dramLatency=1", "-O", "l2.latency=1",
                           "-O", f"aguCount={agus}", "-O", f"dataPorts={ports}"], image))

    # Independent loads expose address-generation and data bandwidth.
    # Dependent loads check that extra ports do not shorten load latency.
    independent = [0x00001fb7]  # lui x31,1: data at 4096 (initially zero)
    independent += [(n % 20 + 1) << 7 | 31 << 15 | 2 << 12 | 0x03 for n in range(96)]
    independent += exit_zero
    single = measure(independent, 1, 1)
    dual = measure(independent, 2, 2)
    assert dual < single, (single, dual)
    # Flat memory's underlying DRAM port still accepts only one request
    # per cycle; widening the core must not bypass that backpressure.
    assert measure(independent, 1, 1, flat=True) == measure(independent, 2, 2, flat=True)

    # At address 1024 put a pointer back to itself, then serially follow it.
    dependent = [0x40000093, 0x0010a023, 0x0000000f]
    dependent += [0x0000a083] * 24  # lw x1,0(x1)
    dependent += exit_zero
    for latency in (1, 7):
        for flat in (False, True):
            assert measure(dependent, 1, 1, latency, flat) == measure(dependent, 2, 2, latency, flat)

print(f"microarchitecture: precise faults and latency preserved; independent loads {single} -> {dual} cycles")
