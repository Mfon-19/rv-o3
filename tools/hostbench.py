#!/usr/bin/env python3
"""Compare host runtime for identical simulated work, with a short cycle limit.

Example:
  python3 tools/hostbench.py --image bench/mm64.bin ./rvsim-before ./rvsim

Run without -d or tracing. Those measure verification overhead as well as the
simulator. Exit 2 is the expected result when the cycle budget runs out.
"""

import argparse
import json
from pathlib import Path
import statistics
import subprocess
import time


def positive(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binaries", nargs="+", type=Path)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--cycles", type=positive, default=2_000_000)
    parser.add_argument("--repeats", type=positive, default=3)
    parser.add_argument("--memory", type=positive, default=64 * 1024 * 1024)
    parser.add_argument("-O", dest="overrides", action="append", default=[])
    parser.add_argument("--timeout", type=positive, default=30)
    parser.add_argument("--json", type=Path, help="save individual timings")
    args = parser.parse_args()

    flags = ["-m", str(args.memory), "-c", str(args.cycles)]
    for setting in args.overrides:
        flags.extend(["-O", setting])
    flags.append(str(args.image.resolve()))

    expected = None
    rows = []
    for repeat in range(args.repeats):
        # Alternate order to reduce bias from temperature and background load.
        order = args.binaries if repeat % 2 == 0 else args.binaries[::-1]
        for binary in order:
            start = time.perf_counter()
            result = subprocess.run(
                [str(binary.resolve()), *flags], stdin=subprocess.DEVNULL,
                capture_output=True, timeout=args.timeout,
            )
            seconds = time.perf_counter() - start
            output = (result.returncode, result.stdout, result.stderr)
            if result.returncode not in (0, 2):
                raise SystemExit(f"{binary} failed:\n{result.stderr.decode(errors='replace')}")
            if expected is None:
                expected = output
            elif output != expected:
                raise SystemExit(f"{binary}: output or simulated statistics changed")
            rows.append({"binary": str(binary), "repeat": repeat, "seconds": seconds})
            print(f"{binary}: {seconds:.4f} s", flush=True)

    baseline = None
    print("\nMedian runtime (all outputs and simulated statistics identical):")
    for binary in args.binaries:
        median = statistics.median(
            row["seconds"] for row in rows if row["binary"] == str(binary)
        )
        if baseline is None:
            baseline = median
        print(f"{binary}: {median:.4f} s, {baseline / median:.3f}x baseline throughput")
    if args.json:
        args.json.write_text(json.dumps({"arguments": flags, "runs": rows}, indent=2) + "\n")


if __name__ == "__main__":
    main()
