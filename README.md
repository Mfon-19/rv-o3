# rv-o3

A cycle-level simulator of a superscalar, out-of-order RISC-V (RV32IM)
processor, written in C++. It models register
renaming, a reorder buffer, oldest-first issue, branch prediction with
full misprediction recovery, speculative load reordering with replay,
and a nonblocking cache hierarchy. Every run can be checked instruction by
instruction against a reference interpreter.

It is fast and complete enough to run **Doom**: the engine, game logic,
and renderer execute as RV32IM instructions on the simulated core, at
**about 5.6 frames per second**.

> Measured on an Apple M4 with the profile-guided build (`make pgo`),
> playing the built-in demo without a window: 177 ms per frame, or
> 5.65 FPS. A normal `make` build gets 5.2 FPS. The simulated CPU is not
> the limit; the frame rate depends on how fast your computer can run the
> simulator. See [doom/README.md](doom/README.md).

## Build and run

```sh
make
./rvsim tests/sum.hex        # prints 55, then a statistics report
./rvsim -d tests/sum.hex     # the same, differentially checked
make test                    # the regression suite
```

Program output goes to stdout; statistics and traces go to stderr. The
simulator needs only a C++20 compiler. The C demo and benchmarks also need
Clang and GNU RISC-V binutils; Doom additionally needs SDL3.

## The machine

```text
     pc, steered by the predictor (bimodal/TAGE + BTB + RAS)
       |
   +-------+   +--------+   +----------------+   +-------------+
   | fetch |-->| decode |-->|    dispatch    |-->| issue queue |
   |       |   | rename |   | ROB, LSQ alloc |   |             |
   +-------+   +--------+   +----------------+   +------+------+
       ^                                            ^    |
       |                                    wakeup: |    | oldest ready
       | redirect: refetch after            results |    | first
       | a mispredict (caught at            ready   |    |
       | writeback) or a load           +-----------+----+----------------+
       | replay (caught in the          | ALU  ALU  branch  mul  div  AGU |
       | LSQ)                           +----+-----------------------+----+
       |                                     | results               | memory ops
       |                                     v                       v
       |                        +---------------------+    +----------------+
       +------------------------| writeback           |<---|      LSQ       |
                                | oldest result first |load|                |
                                +----------+----------+data+--+---------+---+
                                           |                  |         |
                                           v           loads  |         | stores, only
                              commit: in order, the ONLY      |         | after commit
                              architectural update            |         v
                                                              |   +--------------+
                                                              |   | store buffer |
                                                              |   +------+-------+
                                                              v          |
    fetch ---> L1I ---------+----------------- L1D <----------+----------+
                            |                   |
                            +--- unified L2 ----+
                                       |             every cache level:
                                 pipelined DRAM      MSHRs, hit under miss,
                                                     writeback queues
```

Every width, size, and latency is a setting. By default the machine is
two-wide with a 32-entry ROB, 32 KiB L1s, and a 256 KiB L2 (see
`sim/config.h`, or `./rvsim -p`). `configs/doom.cfg` is an eight-wide core with a
128-entry ROB, two address-generation units, a TAGE branch predictor, and a
1 MiB L2.

- **Fetch** reads one aligned block per cycle, steered by a direction
  predictor (bimodal, gshare, or TAGE), a branch target buffer, and a
  return-address stack.
- **Rename** gives every writer a fresh physical register, so
  write-after-write and write-after-read hazards cannot occur; the issue
  queue only waits on true dependences.
- **Execution units**: 1-cycle ALUs, a branch unit, a pipelined
  multiplier, a non-pipelined divider, and address-generation units that
  feed the memory system.
- **Branches** are checked the moment the branch unit resolves them. A
  mispredict flushes everything younger, restores the rename map by walking
  the reorder buffer backwards, and returns the squashed registers. Load
  replays reuse the same recovery.
- **Memory ordering** is a setting (`memOrder`). `conservative` loads
  wait until every older store address is known; `bypass` loads pass
  stores proven not to overlap; `speculative` (the default) loads also pass
  unknown addresses, and if an older store later turns out to overlap,
  the load and everything younger re-execute. Store-to-load forwarding and
  a post-commit store buffer work in every mode.
- **Commit** is the only place architectural state changes. Stores reach
  memory only after commit, syscalls run against a drained machine, and a
  fault on a wrong path is fatal only if its instruction commits, so the
  machine is precise at every instruction boundary.
- **Caches** track outstanding misses in MSHRs, so independent misses
  overlap, requests for a line already on its way merge, and hits keep
  flowing under a miss. Dirty victims wait in writeback queues.

## Three demonstrations

**Out-of-order execution.** Two interleaved, independent dependency
chains sustain more than one instruction per cycle, which no single-issue
in-order machine can do:

```text
$ ./rvsim tests/ilp.hex
--- rvsim: 216 cycles, 249 instructions retired, IPC = 1.153 (CPI = 0.867)
```

**Memory-level parallelism.** Two independent loads miss all the way to
DRAM. With the default four MSHRs the misses overlap; with one they
serialize, and the run gets exactly one miss latency slower:

```text
$ ./rvsim tests/mlp.hex                                   ...77 cycles
$ ./rvsim -O l1d.mshrs=1 -O l2.mshrs=1 tests/mlp.hex      ...107 cycles
```

**A speculative load caught out.** In `tests/replay.hex` a store's
address hides behind a divide while a younger load to the same address
runs early and reads stale data. The `-t` trace shows the cycle the divide
resolves and the machine notices (DS/IS/WB/CT: dispatch, issue, writeback,
commit):

```text
cyc 48 | fq5 rob 6 iq 1 lsq 2 sb1 | IS sw s0,0(sp)  | WB divu sp,t2,t0 ...
cyc 49 | fq5 rob 6 iq 0 lsq 2 sb1 | CT divu sp,t2,t0; addi s0,zero,99   ! load replay @0x00000020
```

The load and everything younger flush, refetch, and this time forward the
correct value; the program prints 100 either way.

## Verification

`core/refmodel.cpp` is a plain fetch-decode-execute interpreter with no
timing. It shares decode and execute semantics with the core, so the two
can only disagree about what the timing model adds: operand routing,
speculation, recovery, and memory ordering. Under `-d` both run in lockstep,
and every retired instruction's effects (pc, register written, memory
written) are compared. At exit the whole register file and memory are
compared too, because out-of-order completion can produce a correct commit
stream while still leaving a stale value behind.

| Target | What it runs |
| --- | --- |
| `make test` | Unit tests (including randomized checks of the issue queue's and LSQ's bookkeeping against brute-force answers), interface tests, and every directed program in `tests/` under `-d` |
| `make randtest` | Generated load/store programs over a few contested cache lines, checked against the reference model |
| `make benchtest` | Each benchmark under `-d` in all three memory-order modes, with output compared byte for byte against a native build of the same C source, an oracle that shares no code with the simulator |

## Benchmarks

Seven small C programs in `bench/`, each stressing one behavior. At the
default configuration:

| Benchmark | IPC | What it shows |
| --- | --- | --- |
| `ptrchase` | 0.440 | Serial pointer chasing over 512 KiB; no window size can help a dependent chain |
| `mlpbench` | 1.001 | Independent gathers over 512 KiB; scales with ROB size and MSHR count |
| `matmul` | 1.673 | 24x24 integer matrix multiply: instruction-level parallelism with quiet caches |
| `mm64` | 1.710 | A 48 KiB working set, just past the 32 KiB L1 |
| `qsortb` | 1.205 | Recursive quicksort: 49 branch mispredicts per 1000 instructions |
| `rle` | 1.581 | Run-length encode/decode: byte traffic in short loops |
| `branchy` | 0.845 | Unpredictable branches: 77 mispredicts per 1000 instructions |

## Configuration

Every setting lives in one struct (`sim/config.h`) and is settable by name.
Errors are fatal, so a typo in a sweep can't silently fall back to a
default.

```sh
./rvsim -p                                   # list every setting and value
./rvsim -O robSize=128 -O memOrder=bypass prog.bin
./rvsim -C configs/doom.cfg prog.bin         # key = value lines, # comments
```

Sizes accept `k`/`m` suffixes (`l1d.size=64k`). The full command line:

```text
usage: ./rvsim [options] [program.hex|program.bin]
  -t            trace pipeline occupancy every cycle (stderr)
  -r            dump registers when the simulation ends
  -d            differential check against the reference model
  -c <cycles>   cycle budget (default 10000000)
  -m <bytes>    memory size (default 1 MiB)
  -C <file>     load configuration ('key = value' lines, # comments)
  -O key=value  override one setting (repeatable; applied after -C)
  -p            print the effective configuration and exit
  --profile F   write per-pc retired instructions, cycles, and
                mispredicts to F (read by tools/guestprof.py)
  --profile-after N  start profiling after N frames were presented
  --frame-fd N, --key-fd N  inherited display pipes (doom/run.py)
```

## Tools

- `make pgo` builds a profile-guided simulator in `build/pgo/rvsim` with
  Clang. Train it on the workload you care about, for example
  `make pgo PGO_IMAGE=doom/build/doom.bin PGO_CONFIG=configs/doom.cfg`.
- `tools/hostbench.py` times simulator binaries on identical simulated work
  and checks that their outputs match. With `--warmup-frames N` it reports
  milliseconds per frame for a graphical guest; this produced the Doom
  measurement above.
- `tools/guestprof.py` turns an `rvsim --profile` file into a
  per-function table of instructions, cycles, and mispredicts.

## Writing programs

Hex programs are whitespace-separated 32-bit instruction words, loaded at
address zero (the reset pc); `#` and `//` start comments. Files ending in
`.bin` load as raw little-endian binaries, so compiler output works too.
`cdemo/` builds a freestanding C program with Clang and runs it
(`make -C cdemo run`); the benchmarks and Doom reuse its toolchain setup.

`ecall` takes a syscall number in `a7` and an argument in `a0`:

| `a7` | Operation |
| --- | --- |
| `1` | print `a0` as a signed decimal integer |
| `2` | print the low byte of `a0` as a character |
| `3` | print the NUL-terminated string at address `a0` |
| `4` | print `a0` as eight hex digits |
| `5` | read one available stdin byte into `a0`, or `0xffffffff` if none |
| `6` | present a framebuffer described at `a0` (`sim/display_protocol.h`) |
| `7` | read a window key event into `a0`, or `0xffffffff` if none |
| `10`, `93` | exit with the low byte of `a0` as the status |

## Layout

```text
isa/      decode, execute semantics, disassembly (shared by core and reference)
core/     the out-of-order core (ooo.cpp), predictor, rename, ROB, issue
          queue, LSQ, functional units, reference model
memory/   caches with MSHRs and writeback queues, pipelined DRAM
sim/      configuration, statistics, loaders, syscalls, display pipe, driver
configs/  the baseline and Doom machines
tests/    directed programs (each header says what it checks), unit tests,
          interface tests, random program generator
bench/    benchmarks with native-build oracles
tools/    hostbench.py (host timing), guestprof.py (guest profiles)
cdemo/    freestanding C demo and its runtime
doom/     the RV32IM Doom port, SDL viewer, and launcher
```

## Limitations

No MMU, interrupts, privileged modes, or trap handling (misaligned and
out-of-range accesses are precise fatal errors); no compressed, atomic, or
floating-point extensions; `fence` is a serializing no-op; one core.
