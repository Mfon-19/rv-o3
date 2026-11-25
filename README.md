# rv-o3

A cycle-level C++20 simulator of a superscalar, out-of-order **RISC-V (RV32IM)** processor (`rvsim`). It models register renaming, a reorder buffer, oldest-first issue, branch prediction (bimodal/gshare/TAGE + BTB + RAS) with full squash recovery, speculative load reordering with replay, and a nonblocking L1I/L1D/L2/DRAM cache hierarchy. Every execution can be verified instruction-by-instruction against a golden functional interpreter (`-d`).

It is fast and complete enough to run **Doom** as an RV32IM guest binary inside the simulated core, streaming rendered frames to a native SDL3 window.

## Build and Run

```sh
make
./rvsim tests/sum.hex         # runs program, prints output + stats
./rvsim -d tests/sum.hex      # runs with lockstep differential verification
make test                     # unit tests + directed .hex programs under -d
```

The simulator requires only a C++20 compiler. Building `bench/` and `cdemo/` requires Clang and GNU RISC-V binutils; running `doom/` additionally requires SDL3 and Python 3.11+.

## Processor & Memory Architecture

```text
                       pc (steered by BTB + RAS + Bimodal / GShare / TAGE)
                                          |
                                          v
    +-----------+    +---------------+    +-------------------+    +-------------+
    |   Fetch   |--->| Fetch Queue   |--->|  Decode & Rename  |--->| Issue Queue |
    | (block)   |    | (Ring buffer) |    |  ROB / LSQ alloc  |    | (oldest-1st)|
    +-----------+    +---------------+    +-------------------+    +------+------+
          ^                                                               ^    |
          |                                                       wakeup: |    | issue ready
          | redirect + flush younger (branch mispredict                   |    | oldest first
          | at writeback, or speculative load replay)                     |    v
          |                                                 +-------------+---------------+
          |                                                 | ALUs | BR | MUL | DIV | AGUs |
          |                                                 +------+----+-----+-----+--+--+
          |                                                        |                   |
          |                                                        v                   v
          |                                             +--------------------+   +------------+
          +---------------------------------------------|     Writeback      |<--|    LSQ     |
                                                        | oldest-first grant |   | fwd/replay |
                                                        +---------+----------+   +---+-----+--+
                                                                  |                  |     |
                                                                  v            loads |     | stores
                                                    +--------------------------+     |     | (commit
                                                    |     Commit (in-order)    |     |     |  only)
                                                    | architectural state,     |     |     v
                                                    | faults, syscalls, retire |     |  +--------------+
                                                    +--------------------------+     |  | Store Buffer |
                                                                                     |  +------+-------+
                                                                                     v         |
    Fetch ---------> [ L1I Cache ] ----+                     +---- [ L1D Cache ] <---+---------+
                                       |                     |
                                       +---> [ Unified L2 ] <+     (all cache levels: MSHRs,
                                                   |                hit-under-miss, miss merging,
                                                   v                LRU, writeback queues)
                                          [ Pipelined DRAM ]
```

### Pipeline Highlights

- **Fetch & Branch Prediction**: Fetches one aligned block per cycle, steered by a set-associative BTB, a Return Address Stack (with optional squash repair), and either a bimodal/gshare PHT or a geometric-history TAGE predictor.
- **Rename & Dispatch**: Allocates fresh physical registers from a free list to eliminate WAW/WAR hazards, and allocates ROB, IQ, and LSQ slots up to `width` instructions per cycle. System operations (`ecall`, `ebreak`, `fence`, `illegal`) serialize on an empty pipeline.
- **Execution**: Configurable 1-cycle ALUs, branch unit, pipelined/non-pipelined multiplier, non-pipelined divider, and 1-cycle AGUs.
- **Memory Ordering (`memOrder`)**:
  - `conservative`: loads wait for all older store addresses and issue in order.
  - `bypass`: loads bypass proven non-overlapping older stores.
  - `speculative` (default): loads bypass unknown store addresses and trigger a pipeline replay if an older store later resolves to an overlapping address.
  - Store-to-load forwarding (from both the LSQ and the post-commit store buffer) works in all modes.
- **Commit & Verification**: Architectural state only updates at commit. Under `-d`, `RefModel` steps once per retired instruction, checking PC, instruction encoding, register writes, memory writes, and final register/RAM state at exit.

## How rv-o3 Works with Doom

```text
                             +---------------------------+
                             |        doom/run.py        |
                             | builds rvsim, doom.bin,   |
                             | doom-viewer & wires pipes |
                             +-------------+-------------+
                                           |
                    spawns both processes with two OS pipes
                      |                                   |
                      v                                   v
  +-----------------------------------------+   +-------------------------------+
  |            rvsim (OoOCore)              |   |    doom-viewer (Native SDL3)  |
  |                                         |   |                               |
  |  Guest RV32IM binary: doom/build/       |   |  Reads RVF1 frame packets     |
  |    doom.bin (64 MiB flat guest RAM)     |   |  from stdin (nonblocking):    |
  |   - Doomgeneric + Picolibc + libgcc     |   |    [magic "RVF1" | w | h |    |
  |   - Embedded read-only game.wad         |   |     w*h XRGB8888 pixels]      |
  |   - Custom memcpy/memset & FixedDiv     |   |                               |
  |                                         |   |  Uploads texture & presents   |
  |  1) Guest renders 320x200 frame into    |   |  4:3 nearest-neighbor window  |
  |     DG_ScreenBuffer (8-bit indexed)     |   |                               |
  |                                         |   |  Captures SDL key down/up     |
  |  2) Guest executes ECALL 6 (present) ---+---+-> --frame-fd pipe ----------> |
  |     rvsim drains pipeline, peeks pixels |   |     (RVF1 XRGB8888 packets)   |
  |     + 256-color palette coherently      |   |                               |
  |     through L1D -> L2 -> DRAM, expands  |   |                               |
  |     to XRGB8888, writes packet          |   |                               |
  |                                         |   |                               |
  |  3) Guest executes ECALL 7 (poll key) <-+---+-- --key-fd pipe <------------ |
  |     rvsim reads nonblocking 32-bit      |   |     (key | (pressed << 8))    |
  |     key event into a0 (or 0xffffffff)   |   |                               |
  +-----------------------------------------+   +-------------------------------+
```

To set up and play Doom (see [doom/README.md](doom/README.md) for full options and controls):

```sh
make -C doom setup
python3 doom/run.py --play
```

## Configuration & CLI

Every core and cache parameter lives in `SimConfig` ([sim/config.h](sim/config.h)) and can be loaded from a file (`-C`) or overridden on the command line (`-O`, repeatable). Sizes accept `k`/`m` suffixes.

```sh
./rvsim -p                                           # print active configuration
./rvsim -C configs/doom.cfg doom/build/doom.bin      # 8-wide TAGE + 1 MiB L2 core
./rvsim -O robSize=128 -O memOrder=bypass prog.bin   # override settings
```

```text
usage: ./rvsim [options] [program.hex|program.bin]
  -t            trace cycle summary (stderr)
  -r            dump registers on exit
  -d            differential check against reference model
  -c <cycles>   cycle budget (default 10000000)
  -m <bytes>    memory size (default 1 MiB)
  -C <file>     load configuration file
  -O key=value  override one setting
  -p            print effective configuration and exit
  --frame-fd N, --key-fd N  display pipes
```

### Guest Syscall ABI (`ecall`)

Syscall number in `a7`, argument/return value in `a0`:

| `a7` | Operation |
| --- | --- |
| `1` | Print `a0` as signed decimal integer |
| `2` | Print low byte of `a0` as a character |
| `3` | Print NUL-terminated string at guest address `a0` |
| `4` | Print `a0` as 8 lowercase hex digits |
| `5` | Nonblocking stdin read into `a0` (or `0xffffffff`) |
| `6` | Present framebuffer descriptor at `a0` ([sim/display_protocol.h](sim/display_protocol.h)) |
| `7` | Nonblocking key event poll into `a0` (or `0xffffffff`) |
| `10`, `93` | Halt and exit with low byte of `a0` |

## Project Layout

```text
isa/      isa.h, isa.cpp               RV32IM decode, execute semantics, disassembly
core/     ooo.h, ooo.cpp               Out-of-order core (rename, ROB, pipeline stages)
          predictor.h, predictor.cpp   BTB, RAS with repair, bimodal/gshare/TAGE
          iq.h, lsq.h, fu.h, ring.h    Issue queue, LSQ/store buffer, FUs, ring buffer
          refmodel.h, refmodel.cpp     CommitRecord and golden reference interpreter
memory/   memory.h                     MemPort/MemRequest/MemResponse, backing RAM, DRAM
          cache.h, cache.cpp           Nonblocking cache with MSHRs and writeback queue
          system.h, system.cpp         L1I/L1D/L2/DRAM hierarchy and coherent peek8
sim/      config.h, config.cpp         SimConfig, file parser, -O overrides, validator
          syscall.h                    Guest ECALL handler and host display pipe I/O
          display_protocol.h           Shared RVF1 frame & key ABI (shared with doom/)
          main.cpp                     CLI driver, .hex/.bin loader, differential harness
configs/  doom.cfg                     8-wide core configuration for Doom
tests/    units.cpp, *.hex             Unit tests and directed RV32IM regression programs
bench/    *.c, bench.h, Makefile       6 freestanding RV32IM microbenchmarks
cdemo/    demo.c, start.S, linker.ld   Freestanding C demo and shared RISC-V toolchain rules
doom/     platform.c, runtime.c, ...   RV32IM Doom port, SDL3 viewer, setup & launcher
```
