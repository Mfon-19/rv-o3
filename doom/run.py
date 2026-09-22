#!/usr/bin/env python3
"""Run the RV32IM guest on the out-of-order core, restoring the terminal on exit."""
import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import termios
import tty

ROOT = Path(__file__).resolve().parent
BUILD = ROOT / "build"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--play", action="store_true", help="start E1M1 with keyboard control")
    parser.add_argument("--frames", type=int, help="stop after N frames (demo default: 120; play: unlimited)")
    parser.add_argument("--diff", action="store_true", help="use the out-of-order core with differential checking")
    parser.add_argument("--plain", action="store_true", help="print frames without ANSI cursor controls")
    parser.add_argument("--columns", type=int, help="ASCII width (default: up to 80, fitted to terminal)")
    parser.add_argument("--rows", type=int, help="ASCII height (default: up to 25, fitted to terminal)")
    parser.add_argument("--budget", type=int, default=100_000_000_000, help="simulation cycle budget")
    parser.add_argument("--wad", type=Path, help="use your own Doom 1 IWAD instead of Freedoom")
    args = parser.parse_args()
    interactive = sys.stdout.isatty() and not args.plain
    size = shutil.get_terminal_size((81, 27))
    cols = args.columns if args.columns is not None else min(80, max(1, size.columns - 1))
    rows = args.rows if args.rows is not None else min(25, max(1, size.lines - 2))
    frames = args.frames if args.frames is not None else (0 if args.play else 120)
    if not 1 <= cols <= 160 or not 1 <= rows <= 100:
        parser.error("columns must be 1..160 and rows must be 1..100")
    if frames < 0 or frames > 0xffffffff or args.budget < 1:
        parser.error("frames must be 0..4294967295 and budget must be positive")
    wad = (args.wad or ROOT / ".deps/freedoom1.wad").resolve()
    if not (ROOT / ".deps/.ready").exists():
        parser.error("run `make -C doom setup` first")
    if not wad.is_file():
        parser.error(f"WAD not found: {wad}")
    if wad.stat().st_size > 48 * 1024 * 1024:
        parser.error("WAD exceeds the guest's 48 MiB data budget")
    BUILD.mkdir(exist_ok=True)
    options = "".join(f"#define {key} {value}\n" for key, value in (
        ("ASCII_COLS", cols), ("ASCII_ROWS", rows), ("MAX_FRAMES", frames),
        ("ANSI_OUTPUT", int(interactive)), ("PLAY_MODE", int(args.play))))
    option_file = BUILD / "options.h"
    if not option_file.exists() or option_file.read_text() != options:
        option_file.write_text(options)
    # Track the chosen WAD as well as its mtime; switching to an older file
    # must still rebuild the embedded image. Use a symlink to handle spaces.
    wad_link = BUILD / "selected.wad"
    if not wad_link.is_symlink() or wad_link.resolve() != wad:
        wad_link.unlink(missing_ok=True)
        wad_link.symlink_to(wad)
        (BUILD / "game.wad").unlink(missing_ok=True)
    build_log = BUILD / "build.log"
    with build_log.open("w") as log:
        try:
            jobs = str(min(os.cpu_count() or 1, 8))
            subprocess.run(["make", "-C", str(ROOT.parent), "-j" + jobs, "rvsim"],
                           stdout=log, stderr=log, check=True)
            subprocess.run(["make", "-C", str(ROOT), "-j" + jobs, "WAD=build/selected.wad", "all"],
                           stdout=log, stderr=log, check=True)
        except subprocess.CalledProcessError:
            print(build_log.read_text()[-8000:], file=sys.stderr)
            return 1
    command = [str(ROOT.parent / "rvsim"), "-m", str(64 * 1024 * 1024),
               "-c", str(args.budget)]
    if args.diff:
        command.append("-d")
    command.append(str(BUILD / "doom.bin"))
    settings = None
    child = None
    stats_path = BUILD / "simulator.log"
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    try:
        if sys.stdin.isatty():
            settings = termios.tcgetattr(sys.stdin.fileno())
            tty.setcbreak(sys.stdin.fileno())
        if interactive:
            sys.stdout.write("\033[?1049h\033[?25l")
            sys.stdout.flush()
        with stats_path.open("w") as stats:
            child = subprocess.Popen(command, stderr=stats)
            code = child.wait()
    except KeyboardInterrupt:
        code = 130
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        if settings is not None:
            termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, settings)
        if interactive:
            sys.stdout.write("\033[?25h\033[?1049l")
            sys.stdout.flush()
    print(stats_path.read_text(), end="", file=sys.stderr)
    return code


if __name__ == "__main__":
    sys.exit(main())
