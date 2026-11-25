# Doom on rv-o3

The Doom engine, game logic, and software renderer run as RV32IM instructions on `rvsim`'s cycle-level out-of-order core, streaming finished frames to a native SDL3 window (`doom-viewer`). Because the full game executes on the simulated CPU, host simulation speed determines the frame rate.

By default, `doom/run.py` launches `rvsim` with [configs/doom.cfg](../configs/doom.cfg): a high-FPS 4-wide out-of-order core tuned for maximum host frame rate (flat 1-cycle multi-port memory, compact queues, dual branch/AGU/data ports, 8192-entry BTB + 16-bit gshare, and bypass memory ordering). Pass `--config default` to run on the built-in 2-wide cached core instead. For even higher FPS, build `rvsim` with `make pgo`.

Three guest-side optimizations reduce simulated instruction count without changing rendered output:

- **Palette expansion in `rvsim`**: Doom renders 8-bit palette indices (`CMAP256` mode) and `rvsim` expands them through the guest's 256-entry palette during `ECALL 6` presentation (`make -C doom CMAP256=0` restores 32-bit guest rendering).
- **Word-at-a-time `memcpy`/`memset`** ([memops.c](memops.c)): replaces Picolibc's byte loops for Doom's 64 KiB framebuffer copies.
- **Fast `FixedDiv`** ([fixed.c](fixed.c)): linked with `--wrap=FixedDiv`, uses two hardware RV32IM 32-bit divides when the divisor fits in 16 bits instead of a 64-bit software divide.

## Setup and Run

Prerequisites: a C++20 compiler, Make, Clang (with RISC-V target), GNU RISC-V binutils (`riscv64-elf-*` on macOS or `riscv64-linux-gnu-*` on Linux), Python 3.11+, `curl`, `dpkg-deb`, SDL3, and `pkg-config`.

From the repository root:

```sh
# macOS prerequisites
brew install sdl3 pkg-config llvm riscv64-elf-binutils dpkg

# Download and verify Doomgeneric, Picolibc, libgcc, and Freedoom into doom/.deps/
make -C doom setup

# Play E1M1 interactively
python3 doom/run.py --play
```

Setup downloads ~230 MB into `doom/.deps/`, verifies SHA-256 checksums, and installs nothing system-wide. Downloads and `doom/build/` are gitignored.

By default, **Freedoom Phase 1** is embedded into `doom/build/doom.bin`. To use your own Doom 1 / Ultimate Doom IWAD:

```sh
python3 doom/run.py --play --wad /path/to/doom.wad
```

On macOS, the toolchain selects Homebrew LLVM Clang automatically (override `CC`, `LLVM_PREFIX`, or `RISCV_PREFIX` via Make if needed).

## Modes and Controls

```sh
# Play the built-in DEMO1 recording for 120 displayed frames
python3 doom/run.py

# Interactive gameplay for a fixed frame count (0 = unlimited)
python3 doom/run.py --play --frames 40

# Differential verification against RefModel on every retired instruction
python3 doom/run.py --diff --frames 1
```

| Input | Action |
| --- | --- |
| `W` / `S` or Up / Down | Move forward / backward |
| `A` / `D` or Left / Right | Turn left / right |
| `J` / `L` | Strafe left / right |
| `F` | Fire |
| `E` or `Space` | Use / open doors |
| `1`–`7` | Select weapon |
| `Esc` / `Enter` | Menu / confirm |
| `Tab` | Automap |
| `Q` or `Ctrl-C` | Exit |

The SDL3 window scales 320×200 with nearest-neighbor filtering and 4:3 letterboxing, delivers press/release key events, and releases held keys on focus loss. The guest uses deterministic virtual time so simulation speed does not cause skipped game ticks. Simulator statistics are written to `doom/build/simulator.log` and printed on exit.

## Architecture & Display Protocol

```text
  doom/build/doom.bin (RV32IM guest, 64 MiB RAM)
    platform.c  -- ECALL 6 (frame) / ECALL 7 (key) --> rvsim (sim/syscall.h)
                                                          |              ^
                                        --frame-fd (RVF1) |              | --key-fd (32-bit key)
                                                          v              |
                                                       doom-viewer (viewer.cpp, SDL3)
```

Defined in [sim/display_protocol.h](../sim/display_protocol.h):

- **Syscall 6 (present frame)**: `a0` points to little-endian words `{pixelAddr, width, height, format[, paletteAddr]}`. Format `1` is `XRGB8888`; format `2` is 8-bit indexed with a 256-entry `XRGB8888` palette. After draining the pipeline, `rvsim` reads pixels coherently through `L1D -> L2 -> DRAM` and writes an `RVF1` packet (`magic`, `width`, `height`, pixels) to `--frame-fd`.
- **Syscall 7 (poll key)**: reads a nonblocking 32-bit event `key | (pressed << 8)` from `--key-fd`, or returns `0xffffffff` when idle. Under `-d`, `RefModel` replays the captured key event and suppresses duplicate frame writes.

### Files

- [platform.c](platform.c): Doomgeneric callbacks, virtual clock, key translation, and entry point.
- [viewer.cpp](viewer.cpp): native SDL3 window reading `RVF1` packets on stdin and writing key events on stdout.
- [runtime.c](runtime.c): Picolibc console/heap stubs and minimal read-only `game.wad` filesystem.
- [wad_io.c](wad_io.c): direct memory-backed WAD lump reader from embedded `_binary_game_wad_start`.
- [memops.c](memops.c), [fixed.c](fixed.c): optimized `memcpy`/`memset` and `FixedDiv`.
- [start.S](start.S), [linker.ld](linker.ld): flat 64 MiB image layout (WAD, Picolibc TLS, heap, 1 MiB stack).
- [setup.py](setup.py), [run.py](run.py): dependency downloader and build/pipe launcher.

## Dependencies and Licenses

- **Doomgeneric** (`dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284`, GPL-2.0): `.deps/doomgeneric/LICENSE`
- **Freedoom** (0.13.0, BSD-style): `.deps/COPYING.txt`
- **Picolibc** (1.8.6-2): `.deps/picolibc/COPYRIGHT`
- **GCC libgcc** (RV32IM, 13.2.0): `.deps/gcc/COPYRIGHT`

Dependencies are downloaded locally rather than vendored. Run `make -C doom clean` to remove build outputs while preserving `doom/.deps/`.
