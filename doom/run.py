#!/usr/bin/env python3
"""Run RV32IM Doom in an SDL window."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent
BUILD = ROOT / "build"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--play", action="store_true", help="start E1M1 with keyboard control")
    parser.add_argument("--frames", type=int, help="stop after N frames (demo default: 120; play: unlimited)")
    parser.add_argument("--diff", action="store_true", help="use the out-of-order core with differential checking")
    parser.add_argument("--budget", type=int, default=100_000_000_000, help="simulation cycle budget")
    parser.add_argument("--wad", type=Path, help="use your own Doom 1 IWAD instead of Freedoom")
    parser.add_argument("--sim", type=Path, help="use an already-built simulator (for example, a PGO build)")
    parser.add_argument("--config", default=str(ROOT.parent / "configs/doom.cfg"),
                        help="core configuration file (default: configs/doom.cfg), "
                             "or 'default' for the simulator's built-in two-wide core")
    args = parser.parse_args()
    frames = args.frames if args.frames is not None else (0 if args.play else 120)
    if frames < 0 or frames > 0xffffffff or args.budget < 1:
        parser.error("frames must be 0..4294967295 and budget must be positive")
    simulator = (args.sim or ROOT.parent / "rvsim").resolve()
    config = None if args.config == "default" else Path(args.config).resolve()
    if config and not config.is_file():
        parser.error(f"configuration not found: {config}")
    if args.sim and (not simulator.is_file() or not os.access(simulator, os.X_OK)):
        parser.error(f"simulator is not an executable file: {simulator}")
    wad = (args.wad or ROOT / ".deps/freedoom1.wad").resolve()
    if not (ROOT / ".deps/.ready").exists():
        parser.error("run `make -C doom setup` first")
    if not wad.is_file():
        parser.error(f"WAD not found: {wad}")
    if wad.stat().st_size > 48 * 1024 * 1024:
        parser.error("WAD exceeds the guest's 48 MiB data budget")
    BUILD.mkdir(exist_ok=True)
    options = "".join(f"#define {key} {value}\n" for key, value in (
        ("MAX_FRAMES", frames), ("PLAY_MODE", int(args.play))))
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
            if args.sim is None:
                subprocess.run(["make", "-C", str(ROOT.parent), "-j" + jobs, "rvsim"],
                               stdout=log, stderr=log, check=True)
            subprocess.run(["make", "-C", str(ROOT), "-j" + jobs, "WAD=build/selected.wad", "all"],
                           stdout=log, stderr=log, check=True)
            subprocess.run(["make", "-C", str(ROOT), "viewer"],
                           stdout=log, stderr=log, check=True)
        except subprocess.CalledProcessError:
            print(build_log.read_text()[-8000:], file=sys.stderr)
            return 1
    command = [str(simulator), "-m", str(64 * 1024 * 1024),
               "-c", str(args.budget)]
    if config:
        command += ["-C", str(config)]
    if args.diff:
        command.append("-d")
    command.append(str(BUILD / "doom.bin"))
    child = None
    viewer = None
    pipe_fds = []
    stats_path = BUILD / "simulator.log"
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    try:
        with stats_path.open("w") as stats:
            frame_read, frame_write = os.pipe()
            key_read, key_write = os.pipe()
            pipe_fds = [frame_read, frame_write, key_read, key_write]
            viewer = subprocess.Popen([str(BUILD / "doom-viewer")],
                                      stdin=frame_read, stdout=key_write)
            command[1:1] = ["--frame-fd", str(frame_write), "--key-fd", str(key_read)]
            child = subprocess.Popen(command, stderr=stats, stdin=subprocess.DEVNULL,
                                     pass_fds=(frame_write, key_read))
            for fd in pipe_fds:
                os.close(fd)
            pipe_fds.clear()
            while child.poll() is None and viewer.poll() is None:
                time.sleep(0.05)
            if child.poll() is None:
                # Closing the window interrupts even a long simulated frame.
                code = viewer.returncode
            else:
                code = child.returncode
                # EOF lets the viewer show the last complete frame and exit.
                try:
                    viewer_code = viewer.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    viewer_code = 1
                if code == 0 and viewer_code:
                    code = viewer_code
    except KeyboardInterrupt:
        code = 130
    finally:
        for fd in pipe_fds:
            os.close(fd)
        for process in (child, viewer):
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    print(stats_path.read_text(), end="", file=sys.stderr)
    return code


if __name__ == "__main__":
    sys.exit(main())
