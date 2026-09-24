# Doom on rv-o3

The Doom engine, game logic, and software renderer execute as RV32IM
instructions on `rvsim`'s cycle-level out-of-order core; an SDL3 window
shows the finished frames. Everything runs on the simulated CPU, so
simulation speed sets the frame rate: about 5.6 FPS with the
profile-guided simulator on an Apple M4 (measurement details in the
[main README](../README.md)).

The launcher uses `configs/doom.cfg`: an eight-wide core with a 128-entry
ROB, two address-generation units, two data accesses per cycle, 64-byte
fetch, a TAGE branch predictor, and a 1 MiB L2. Over frames 40–100 of the
bundled 100-frame demo it runs at 4.08 instructions per cycle (130.25M
instructions in 31.96M cycles). `--config default` runs the simulator's
default two-wide core instead.

Three changes make the guest itself cheaper to simulate, without changing
what it draws:

- Doom keeps its 8-bit palette indices (doomgeneric's `CMAP256` mode) and
  `rvsim` expands them through the guest's palette when it presents a
  frame, as a display controller's palette hardware would. Converting on
  the guest took about 31 instructions per pixel, nearly half of each
  frame's instructions. `make -C doom CMAP256=0` restores that path.
- `memops.c` replaces picolibc's byte-at-a-time `memcpy`/`memset` with
  word-at-a-time versions; Doom copies a 64 KB frame every tic.
- `fixed.c` replaces `FixedDiv` (linked with `--wrap`) with a bit-identical
  version that uses two 32-bit divides instead of a 64-bit software divide
  when the divisor fits in 16 bits.

## Start

From the repository root:

```sh
# macOS prerequisites (the simulator itself only needs a C++20 compiler)
brew install sdl3 pkg-config llvm riscv64-elf-binutils dpkg
make -C doom setup
python3 doom/run.py --play
```

Prerequisites: a C++20 compiler, Make, Clang, GNU RISC-V
binutils (`riscv64-linux-gnu-*` on Linux or `riscv64-elf-*` on Homebrew),
Python 3.11 or newer, `curl`, and `dpkg-deb`. The launcher requires a POSIX
host with a graphical display, SDL3, and `pkg-config`. The normal `rvsim`
build has no SDL dependency. Setup downloads about 230 MB into `doom/.deps`,
checks SHA-256 hashes, and extracts only the embedded-library variant we
use. It installs no system packages and needs no root access. Downloads
and build outputs are ignored by Git.

On macOS the guest build selects Homebrew's LLVM Clang automatically;
Apple's system Clang does not include the RISC-V backend. Override `CC`,
`LLVM_PREFIX`, or `RISCV_PREFIX` through Make if your toolchain lives elsewhere.

The viewer displays the 320x200 image in a resizable window,
with nearest-neighbor scaling and 4:3 aspect correction.

The default is **Freedoom Phase 1**, freely licensed game data for the Doom
engine. To use your own Doom 1/Ultimate Doom IWAD:

```sh
python3 doom/run.py --play --wad /path/to/doom.wad
```

The guest has 64 MiB of RAM. The IWAD is embedded in the binary; changing
it relinks the image. This port supports one read-only IWAD and no PWADs,
save files, config persistence, sound, or networking.

For the fastest frame rate, build a profile-guided simulator trained on
Doom (build the Doom image with the launcher first), then play with it:

```sh
make pgo PGO_IMAGE=doom/build/doom.bin PGO_CONFIG=configs/doom.cfg
python3 doom/run.py --play --sim build/pgo/rvsim
```

`--sim` uses the given executable as-is, so rebuild it after changing the
simulator. Training runs headless with a cycle limit.

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

The SDL window delivers real key-down and key-up events, supports held
and simultaneous keys, and releases held keys when focus is lost. Closing
the window or pressing Q stops the simulator, even between guest frames.

The opening screen wipe may briefly show a texture before the level
becomes visible.

```sh
# Play the built-in DEMO1 recording for 120 displayed frames.
python3 doom/run.py

# Run a short gameplay session.
python3 doom/run.py --play --frames 40

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
Closing the window interrupts the simulator immediately; use a frame limit
or Doom's menu quit for a completed run with final statistics and `--diff`
final-state verification.

## Implementation

- `platform.c`: Doomgeneric callbacks, frame presentation, virtual clock,
  keyboard mapping, and demo/play entry point.
- `viewer.cpp`: native SDL3 window and keyboard loop, independent of simulation
  progress. The viewer reads framed pixel packets and writes key events through
  separate pipes; text output and statistics never enter the pixel stream.
- `runtime.c`: Picolibc console/heap hooks and a small read-only filesystem
  exposing only the embedded `game.wad`.
- `wad_io.c`: bulk guest-memory WAD reads into Doom's aligned lump cache.
- `memops.c`, `fixed.c`: the faster `memcpy`/`memset` and `FixedDiv`.
- `start.S` / `linker.ld`: flat image startup, TLS, heap, and 1 MiB stack.
- `setup.py`: pinned, checksummed dependencies; `run.py`: build and launch.

The display ABI is documented in `sim/display_protocol.h`:

- **6**: present a frame. `a0` points to little-endian words: framebuffer
  address, width, height, and format. Format `1` is 32-bit `0x00RRGGBB`
  pixels; format `2` is one byte per pixel indexing a 256-entry
  `0x00RRGGBB` palette whose address is a fifth word. The simulator checks
  bounds and reads through its cache hierarchy after the serializing ECALL
  has drained older work. Presentation is host I/O: it models no GPU and
  adds no simulated memory traffic.
- **7**: nonblocking window event, encoded as `key | (pressed << 8)`, or
  `UINT32_MAX`. Keys are ASCII plus the four arrow codes in the shared header.

The launcher supplies `rvsim --frame-fd N --key-fd N` with inherited pipe
descriptors. During `-d`, the reference model replays window input from
syscall 7 and suppresses duplicate frame output.

`make test` includes the display checks (cache visibility, both pixel
formats, input replay, malformed descriptors), which need no SDL.

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
