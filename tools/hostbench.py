#!/usr/bin/env python3
"""Compare host runtime for identical simulated work, with a short cycle limit.

Example:
  python3 tools/hostbench.py --image bench/mm64.bin ./rvsim-before ./rvsim

Run without -d or tracing. Those measure verification overhead as well as the
simulator. Exit 2 is the expected result when the cycle budget runs out.
Graphical guests are supported: frames are discarded and no keys are supplied.
Use --warmup-frames 40 to measure completed-frame throughput as well. Supply
an image with a finite frame limit and enough --cycles to let it exit.
"""

import argparse
import hashlib
import os
from pathlib import Path
import statistics
import struct
import subprocess
import threading
import time


def positive(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def framed_run(command, flags, timeout, start):
    """Drain the framebuffer concurrently with stdout/stderr, without SDL."""
    frame_times, errors = [], []
    digest = hashlib.sha256()
    rd, wr = os.pipe()
    with os.fdopen(rd, "rb") as frames, os.fdopen(wr, "wb") as output, \
            open(os.devnull, "rb") as keys:
        process = subprocess.Popen(
            [command, "--frame-fd", str(wr), "--key-fd", str(keys.fileno()), *flags],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            pass_fds=(wr, keys.fileno()),
        )
        output.close()  # only the child owns the writer now; exit delivers EOF

        def consume():
            try:
                while header := frames.read(12):
                    if len(header) != 12:
                        raise ValueError("truncated frame header")
                    magic, width, height = struct.unpack("<III", header)
                    if magic != 0x31465652 or not (0 < width <= 640 and 0 < height <= 480):
                        raise ValueError("invalid frame header")
                    pixels = frames.read(width * height * 4)
                    if len(pixels) != width * height * 4:
                        raise ValueError("truncated frame pixels")
                    digest.update(header)
                    digest.update(pixels)
                    frame_times.append(time.perf_counter() - start)
            except Exception as error:
                errors.append(error)
                process.kill()

        reader = threading.Thread(target=consume)
        reader.start()
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
            reader.join()
        if errors:
            raise SystemExit(f"frame capture failed: {errors[0]}")
        result = subprocess.CompletedProcess(process.args, process.returncode, stdout, stderr)
    return result, frame_times, digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binaries", nargs="+", type=Path)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--cycles", type=positive, default=2_000_000)
    parser.add_argument("--repeats", type=positive, default=3)
    parser.add_argument("--memory", type=positive, default=64 * 1024 * 1024)
    parser.add_argument("--config", type=Path, help="same core configuration for every binary")
    parser.add_argument("--warmup-frames", type=positive,
                        help="measure frame intervals after N warm-up frames (headless)")
    parser.add_argument("-O", dest="overrides", action="append", default=[])
    parser.add_argument("--timeout", type=positive, default=30)
    args = parser.parse_args()

    flags = ["-m", str(args.memory), "-c", str(args.cycles)]
    if args.config:
        flags.extend(["-C", str(args.config.resolve())])
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
            frame_times, frame_hash = [], None
            if args.warmup_frames is not None:
                result, frame_times, frame_hash = framed_run(
                    str(binary.resolve()), flags, args.timeout, start)
                if result.returncode != 0 or len(frame_times) <= args.warmup_frames:
                    raise SystemExit(f"{binary}: frame benchmark needs a completed run with "
                                     f"more than {args.warmup_frames} frames:\n"
                                     + result.stderr.decode(errors="replace"))
            else:
                with open(os.devnull, "wb") as frames, open(os.devnull, "rb") as keys:
                    result = subprocess.run(
                        [str(binary.resolve()), "--frame-fd", str(frames.fileno()),
                         "--key-fd", str(keys.fileno()), *flags], stdin=subprocess.DEVNULL,
                        pass_fds=(frames.fileno(), keys.fileno()),
                        capture_output=True, timeout=args.timeout,
                    )
            seconds = time.perf_counter() - start
            output = (result.returncode, result.stdout, result.stderr, frame_hash)
            if result.returncode not in (0, 2):
                raise SystemExit(f"{binary} failed:\n{result.stderr.decode(errors='replace')}")
            if expected is None:
                expected = output
            elif output != expected:
                raise SystemExit(f"{binary}: output or simulated statistics changed")
            row = {"binary": str(binary), "repeat": repeat, "seconds": seconds}
            message = f"{binary}: {seconds:.4f} s"
            if frame_times:
                intervals = len(frame_times) - args.warmup_frames
                elapsed = frame_times[-1] - frame_times[args.warmup_frames - 1]
                row.update(frames=len(frame_times), frame_sha256=frame_hash,
                           frame_times=frame_times, fps=intervals / elapsed,
                           ms_per_frame=1000 * elapsed / intervals)
                message += f", {row['ms_per_frame']:.2f} ms/frame ({row['fps']:.3f} FPS)"
            rows.append(row)
            print(message, flush=True)

    baseline = None
    print("\nMedian runtime (all outputs and simulated statistics identical):")
    for binary in args.binaries:
        median = statistics.median(
            row["seconds"] for row in rows if row["binary"] == str(binary)
        )
        if baseline is None:
            baseline = median
        print(f"{binary}: {median:.4f} s, {baseline / median:.3f}x baseline throughput")
        if args.warmup_frames is not None:
            ms = statistics.median(row["ms_per_frame"] for row in rows
                                   if row["binary"] == str(binary))
            print(f"  after frame {args.warmup_frames}: {ms:.2f} ms/frame, {1000/ms:.3f} FPS")


if __name__ == "__main__":
    main()
