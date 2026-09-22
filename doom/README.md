# ASCII Doom on rv-o3

The Doom engine, game logic, software renderer, and RGB-to-ASCII conversion
all execute as RV32IM instructions on `rvsim`'s cycle-level out-of-order
core. The host prints the characters and supplies keyboard bytes. This
works in an ordinary SSH
terminal without X11, Wayland, SDL, or a GPU.

## Start

From the repository root:

```sh
make -C doom setup
python3 doom/run.py --play
```

Prerequisites: the project's C++17 compiler, Make, Clang, GNU RISC-V
binutils (`riscv64-linux-gnu-ld` and `riscv64-linux-gnu-objcopy`),
Python 3.11 or newer, `curl`, and `dpkg-deb`. The launcher requires a POSIX
host with terminal support. Setup downloads about 230 MB into `doom/.deps`,
checks SHA-256 hashes, and extracts only the embedded-library variant we
use. It installs no system packages and needs no root access. Downloads
and build outputs are ignored by Git.

The default is **Freedoom Phase 1**, freely licensed game data for the Doom
engine. To use your own Doom 1/Ultimate Doom IWAD:

```sh
python3 doom/run.py --play --wad /path/to/doom.wad
```

The guest has 64 MiB of RAM. The IWAD is embedded in the binary; changing
it relinks the image. This port supports one read-only IWAD and no PWADs,
save files, config persistence, sound, or networking.

## Controls and modes

| Input | Action |
| --- | --- |
| W/S or up/down arrows | Forward/backward |
| A/D or left/right arrows | Turn |
| J/L | Strafe left/right |
| F | Fire |
| E or Space | Use/open doors |
| 1–7 | Select weapon |
| Escape / Enter | Menu / confirm |
| Tab | Automap |
| Q or Ctrl-C | Exit |

Terminals report key presses and repeats, not releases. The port holds
each key for two rendered frames and renews it on repeats; simultaneous
keys are less precise than a graphical game window. The opening screen
wipe may briefly show a texture before the level becomes visible.

```sh
# Play the built-in DEMO1 recording for 120 displayed frames.
python3 doom/run.py

# Keep the frames as plain text, with no cursor escape sequences.
python3 doom/run.py --plain --frames 40 > frames.txt

# Control character resolution (normally up to 80x25, fitted to terminal).
python3 doom/run.py --play --columns 100 --rows 30

# Also check every retirement and final register/memory state.
python3 doom/run.py --diff --frames 1
```

`--frames 0` runs without a frame limit. `--budget N` sets the simulation
cycle budget. A frame limit includes the opening wipe frames. Use at least
40 frames for a
short capture that shows the level. The engine uses deterministic virtual
time, so a slow simulator does not accumulate real-time game ticks.

Every run simulates pipeline stages and caches on the out-of-order core.
The reference interpreter is used only for verification with `--diff`.
Stats are saved to `doom/build/simulator.log` and printed on exit; build
details go to `doom/build/build.log`.

Terminal mode uses an alternate screen, hides the cursor, and disables
line buffering/echo. Normal exit, Ctrl-C, and SIGTERM restore the terminal.
Plain output can be redirected or piped. As with any terminal application,
SIGKILL cannot run cleanup; `stty sane` restores input settings if needed.

## Implementation

- `platform.c`: Doomgeneric callbacks, grayscale ASCII conversion, virtual
  clock, keyboard mapping, and bounded demo/play entry point.
- `runtime.c`: Picolibc console/heap hooks and a small read-only filesystem
  exposing only the embedded `game.wad`.
- `wad_io.c`: bulk guest-memory WAD reads into Doom's aligned lump cache.
- `start.S` / `linker.ld`: flat image startup, TLS, heap, and 1 MiB stack.
- `setup.py`: pinned, checksummed dependencies; `run.py`: build and launch.

The only added simulator service is syscall **5**, a nonblocking stdin
read returning one unsigned byte or `UINT32_MAX` in `a0`. It executes at
the serializing ECALL boundary. During `-d`, the reference model replays
the primary core's byte instead of reading stdin twice. Existing syscall
3 prints each completed ASCII frame.

`make testinput` covers bytes (including 255), EOF, an empty open pipe,
dependent instructions, and replay in both default and wider cores.
`make test` includes those checks and the existing directed programs.

## Dependencies and licenses

- [Doomgeneric](https://github.com/ozkl/doomgeneric), revision
  `dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284`, GPL-2.0; its license is kept
  at `.deps/doomgeneric/LICENSE` and individual source notices also apply.
- [Freedoom](https://github.com/freedoom/freedoom), version 0.13.0,
  BSD-style license retained at `.deps/COPYING.txt`.
- [Picolibc](https://github.com/picolibc/picolibc), Ubuntu package 1.8.6-2,
  copyright/license notices retained at `.deps/picolibc/COPYRIGHT`.
- GCC's RV32IM software arithmetic library, Ubuntu package
  13.2.0-11ubuntu1+12; notices at `.deps/gcc/COPYRIGHT`, including the
  GCC Runtime Library Exception.

The engine is fetched, not vendored. Preserve the relevant licenses and
provide the corresponding source as required if redistributing a built
Doom executable. `make -C doom clean` removes build outputs while keeping
the downloaded dependencies.
