#!/usr/bin/env python3
"""Summarize an rvsim --profile file by guest function.

Example:
  rvsim --profile doom.prof --profile-after 40 ... doom/build/doom.bin
  python3 tools/guestprof.py doom.prof doom/build/doom.elf

Retired counts are exact. Cycles charge each retirement gap to the
instruction that ended it, so they show where retirement waited, not
where every cycle of work happened. Mispredicts count retired branches
that fetch guessed wrong (wrong-path work never retires).
"""
import argparse
import bisect
import os
import shutil
import subprocess
import sys


def find_nm():
    prefix = os.environ.get("RISCV_PREFIX")
    names = [f"{prefix}nm"] if prefix else []
    names += ["riscv64-elf-nm", "riscv64-linux-gnu-nm", "llvm-nm"]
    for name in names:
        if shutil.which(name):
            return name
    sys.exit("no nm found; set RISCV_PREFIX")


def load_symbols(elf):
    out = subprocess.run([find_nm(), "-n", elf], check=True,
                         capture_output=True, text=True).stdout
    starts, names = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tTwW":
            starts.append(int(parts[0], 16))
            names.append(parts[2])
    return starts, names


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("profile")
    parser.add_argument("elf")
    parser.add_argument("-n", "--top", type=int, default=30)
    parser.add_argument("--sort", choices=("retired", "cycles", "mispredicts"),
                        default="retired")
    args = parser.parse_args()

    starts, names = load_symbols(args.elf)
    header, totals = "", {}
    with open(args.profile) as f:
        for line in f:
            if line.startswith("#"):
                header = header or line[1:].strip()
                continue
            pc, *counts = line.split()
            counts = [int(x) for x in counts] + [0] * (3 - len(counts))
            i = bisect.bisect_right(starts, int(pc, 16)) - 1
            name = names[i] if i >= 0 else "?"
            totals[name] = [t + x for t, x in zip(totals.get(name, [0, 0, 0]), counts)]

    all_r, all_c, all_m = (sum(t[k] for t in totals.values()) or 1 for k in range(3))
    key = ("retired", "cycles", "mispredicts").index(args.sort)
    print(header)
    print(f"{'retired':>13} {'%':>6} {'cum%':>6} {'cycles%':>8} {'IPC':>5} "
          f"{'mispred':>10} {'%':>6}  function")
    cum = 0
    for name, (r, c, m) in sorted(totals.items(), key=lambda kv: -kv[1][key])[:args.top]:
        cum += r
        ipc = f"{r / c:5.2f}" if c else "    -"
        print(f"{r:13,} {100 * r / all_r:6.2f} {100 * cum / all_r:6.2f} "
              f"{100 * c / all_c:8.2f} {ipc} {m:10,} {100 * m / all_m:6.2f}  {name}")


if __name__ == "__main__":
    main()
