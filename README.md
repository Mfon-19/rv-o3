# rv-o3

This is a cycle-level simulator of a superscalar, out-of-order RISC-V processor that implements RV32IM. The machine is currently a 2-wide superscalar with register renaming, a reorder buffer, oldest-ready issue, branch prediction with full misprediction recovery, a nonblocking cache hierarchy with memory-level parallelism, and speculative load reordering with replay. I made this after studying computer architecture as a learning exercise.

## Build and run

```sh
make
./rvsim tests/sum.hex        # runs; prints 55 plus a stats report
./rvsim -d tests/sum.hex     # same, differentially checked (see below)
make test                    # the full directed suite under -d
```

The program's output goes to standard output; statistics and traces
go to standard error. No libraries are needed beyond a C++17
compiler; the C demo and benchmarks additionally want clang and GNU
RISC-V binutils.

## The machine at a glance

```text
            bimodal + BTB + RAS
                  |
   +-------+   +--------+   +----------------+   +-------------+
   | fetch |-->| decode |-->|    dispatch    |-->| issue queue |
   |2/cycle|   | rename |   | ROB, LSQ alloc |   |  16 entries |
   +-------+   +--------+   +----------------+   +------+------+
                                                        | oldest ready,
                                                        | 2/cycle
              +-------+-------+--------+-------+-------++
              |  ALU  |  ALU  | branch |  mul  |  div  | AGU
              +-------+-------+--------+-------+-------+  |
                              |                           v
                    writeback, 2 ports          load/store queue (16)
                    wakes the issue queue       + store buffer (8)
                              |                           |
                              v                           | loads and
                    commit, in order, 2/cycle             | committed
                    the ONLY architectural update         | stores
                                                          v
        fetch --> L1I 32K ----+----------------- L1D 32K <--+
                              |                    |
                              +---- unified L2 256K
                                         |          every cache level:
                                   pipelined DRAM   MSHRs, hit under miss,
                                   (30 cycles)      writeback queues
```

- **Fetch** reads an aligned 8-byte block each cycle (two instructions), steered by a bimodal
  direction table, a branch target buffer, and a return-address
  stack.
- **Rename** gives every writer a fresh physical register (32
  architectural plus one per ROB entry). Write-after-write and
  write-after-read hazards cease to exist at this line; the issue
  queue only ever waits on true dependences.
- **Execution units**: two 1-cycle ALUs, a 1-cycle branch unit, a
  3-cycle pipelined multiplier, a 12-cycle non-pipelined divider, and
  a 1-cycle address-generation unit feeding the memory system.
- **Branches** are checked against their prediction the moment the branch unit produces the real outcome; a mispredict flushes everything
  younger, restores the rename map by walking the reorder buffer
  backwards, and returns the squashed physical registers. Recovery is
  also reused for memory replays.
- **Memory ordering** is a policy configuration (`memOrder`). In
  `conservative` mode loads wait until every older store address is
  known; `bypass` lets loads pass stores proven not to overlap;
  `speculative` (the default) lets loads pass unknown addresses too,
  and when an older store later resolves to the same address, the
  load and everything younger flush and re-execute. Store-to-load
  forwarding and a post-commit store buffer work in every mode.
- **Commit** is the only place architectural state changes. Stores
  reach memory only after commit; syscalls execute against a drained
  machine; a fault on a speculative path becomes fatal only if its
  instruction commits. The machine is precise at every boundary.
- **Caches** track outstanding misses in MSHRs, so independent misses
  overlap, duplicate requests merge, and hits keep flowing under a
  miss; dirty victims wait in writeback queues; DRAM is pipelined.

## Three demonstrations

**Out-of-order execution** Two interleaved independent
dependency chains sustain more than one instruction per cycle, which
no in-order machine can do:

```text
$ ./rvsim tests/ilp.hex
--- rvsim: 216 cycles, 249 instructions retired, IPC = 1.153 (CPI = 0.867)
```

**Memory-level parallelism, measured with a config override.** Two
independent loads miss all the way to DRAM. With the default four
MSHRs the misses overlap; with one MSHR they serialize, and the run
gets exactly one miss latency slower:

```text
$ ./rvsim tests/mlp.hex                                   ...77 cycles
$ ./rvsim -O l1d.mshrs=1 -O l2.mshrs=1 tests/mlp.hex      ...107 cycles
```

**A speculative load being caught out.** In `tests/replay.hex` a
store's address hides behind a divide while a younger load to the
same address runs early and reads stale data. The `-t` trace shows
the moment the divide resolves and the machine notices (DS/IS/WB/CT are dispatch, issue, writeback, commit):

```text
cyc 48 | fq5 rob 6 iq 1 lsq 2 sb1 | IS sw s0,0(sp)  | WB divu sp,t2,t0 ...
cyc 49 | fq5 rob 6 iq 0 lsq 2 sb1 | CT divu sp,t2,t0; addi s0,zero,99   ! load replay @0x00000020
```

The load and everything younger flush, refetch, and this time forward
the correct value; the program prints 100 either way.

## Verification

`core/refmodel.cpp` is a fetch-decode-execute interpreter with no
timing at all. It shares the decode and execute semantics with the
core, so the two can only diverge in what the timing model adds:
operand routing, speculation, recovery, memory ordering. Under `-d`,
both run in lockstep and every retired instruction's effects (pc,
register written, memory written) are compared record by record; at
exit the full register file and all of memory are compared as well,
because out-of-order completion can produce a commit stream that is
correct record by record while still leaving a stale value in a
register.

Four make targets build on the checker:

| Target | What it does |
| --- | --- |
| `make test` | 18 directed programs, each aimed at one mechanism, plus the compiled C demo |
| `make randtest` | Generated load/store soups over a few contested cache lines, with late-resolving addresses and eviction pressure; the reference model is the oracle |
| `make configtest` | The directed suite under twelve configurations: widths 1 and 4, ROB 16 and 128, one MSHR, all three ordering modes, flat memory, tiny caches, and more |
| `make benchtest` | Every benchmark is also compiled natively for the host; simulator output must match the native binary byte for byte, in all three ordering modes. The host build shares no code with the simulator, so it checks the one layer the shared reference model cannot |

## Benchmarks

Seven small C programs in `bench/`, each stressing one behavior, and
with a native host build as its oracle. At the default configuration:

| Benchmark | IPC | What it shows |
| --- | --- | --- |
| `ptrchase` | 0.44 | serial pointer chasing over 512 KiB; about 0.9 misses outstanding, and no window size can help a serial chain |
| `mlpbench` | 1.00 | independent gathers over 512 KiB; 1.5 misses outstanding, scaling with ROB size and MSHR count |
| `matmul` | 1.67 | 24x24 integer matrix multiply; instruction-level parallelism with quiet caches |
| `mm64` | 1.70 | 48 KiB working set; the L1-capacity knee (8% L1D misses at 16 KiB, 0.4% at 64 KiB) |
| `qsortb` | 1.20 | recursive quicksort; 49 branch MPKI of data-dependent compares |
| `rle` | 1.63 | encode/decode round trip; byte traffic in short loops |
| `branchy` | 0.85 | genuinely unpredictable branches; 77 MPKI and constant recovery |

`tools/sweep.py` runs them across one axis of the design space
(`rob`, `width`, `l1`, `mshr`, `pred`) and prints markdown tables of
IPC plus the metric that axis is supposed to move.

## Configuration

Every config lives in one struct and is settable by name.

```sh
./rvsim -p                                  # list every config and value
./rvsim -O robSize=128 -O memOrder=bypass prog.bin
./rvsim -C experiment.txt prog.bin          # key = value lines, # comments
```

Sizes take `k`/`m` suffixes (`l1d.size=64k`). `physRegs=0` (the
default) derives 32 plus the ROB size. The full command line:

```text
usage: ./rvsim [options] [program.hex|program.bin]
  -t            trace pipeline occupancy every cycle (stderr)
  -r            dump registers when the simulation ends
  -f            run the functional reference model (no pipeline)
  -d            differential check against the reference model
  -c <cycles>   cycle budget (default 10000000)
  -m <bytes>    memory size (default 1 MiB)
  -C <file>     load configuration
  -O key=value  override one knob (repeatable; applied after -C)
  -p            print the effective configuration and exit
```

## Writing programs

Hex programs are plain text: whitespace-separated 32-bit instruction
words, loaded from address zero, which is also the reset `pc`. Both
`#` and `//` start comments, so a program reads like a listing:

```text
00500093   # addi ra, zero, 5
00008513   # addi a0, ra, 0
00100893   # addi a7, zero, 1    syscall 1: print integer
00000073   # ecall
05d00893   # addi a7, zero, 93   syscall 93: exit
00000073   # ecall
```

Files ending in `.bin` load as raw little-endian binaries instead, so
assembler or compiler output works too. `ecall` reads a syscall
number from `a7` and an argument from `a0`; system instructions
drain the machine first and execute at commit, so they always see
settled state.

| `a7` | Operation |
| --- | --- |
| `1` | print `a0` as a signed decimal integer |
| `2` | print the low byte of `a0` as a character |
| `3` | print the NUL-terminated string at address `a0` |
| `4` | print `a0` as eight hex digits |
| `10`, `93` | exit with the low byte of `a0` as the status |

### Running real C

`cdemo/` compiles a freestanding C program (sorting, GCD, string
output) with clang for bare-metal RV32IM and runs it on the
simulator: `make -C cdemo run`. There is no operating system or libc
underneath, so the directory supplies the pieces needed:
`start.S` (entry point and stack setup), `runtime.c` (`memcpy` and
`memset` for compiler-generated calls), and `linker.ld` (final
addresses, code at zero).`make -C cdemo disassemble`
shows the exact instructions your compiler produced. The benchmarks
in `bench/` reuse the same runtime.

## Layout

```text
isa/        decode, execute semantics, disassembly; timing-free and
            shared by the core and the reference model, so their
            architectural behavior cannot diverge
core/       ooo.{h,cpp} (the machine), predictor, rename, rob, iq,
            lsq, fu, refmodel, commit records
memory/     tagged nonblocking ports, cache (MSHRs, writeback
            queues), pipelined DRAM, hierarchy assembly
sim/        config (struct + file/override interface), stats, program
            loaders, CLI driver with the differential checker
tests/      18 annotated directed tests plus randgen.py
bench/      benchmarks with native-build oracles
tools/      configtest.sh, sweep.py
cdemo/      freestanding C demo and its runtime
```

## Limitations

Out-of-scope features: no
MMU, interrupts, privileged modes, or trap handling (misaligned and
out-of-bounds accesses are precise fatal errors); no compressed,
atomic, or floating-point extensions; `fence` is a serializing no-op;
one core.
