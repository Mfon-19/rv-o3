# RISC-V Simulator

This is a cycle-level simulator of the classic five-stage RISC-V pipeline
(`IF -> ID -> EX -> MEM -> WB`), implementing the base RV32I ISA plus the M
extension for multiplication and division. The whole simulator is one
readable C++ file with no dependencies. It exists to make pipeline
behavior *visible*: forwarding, load-use stalls, branch squashes, and
control-flow redirects can all be watched happening cycle by cycle.

## Quick start

```sh
make
./rvsim tests/sum.hex
```

```text
--- rvsim: 61 cycles, 39 instructions retired, CPI = 1.564
--- rvsim: 0 load-use stalls, 9 taken branches/jumps (18 squashed instructions)
--- rvsim: exit code 0
55
```

The program's own output (`55`, the sum of 1..10) goes to standard output;
statistics go to standard error.

## Watching the pipeline

The `-t` flag prints the occupancy of every stage each cycle, with
annotations when something interesting happens. Here is
`./rvsim -t tests/hazards.hex` catching a load-use hazard: `add tp,gp,gp`
needs the result of `lw gp,0(sp)` one cycle before forwarding can deliver
it, so the pipeline injects a single bubble and replays the `add`:

```text
cyc  5 | IF 0x00000010 | ID lw gp,0(sp)    | EX sw ra,0(sp)    | MEM addi ra,zero,42 | WB lui sp,0x1
cyc  6 | IF 0x00000014 | ID add tp,gp,gp   | EX lw gp,0(sp)    | MEM sw ra,0(sp)     | WB addi ra,zero,42   ! load-use stall (bubble -> EX)
cyc  7 | IF 0x00000014 | ID add tp,gp,gp   | EX -              | MEM lw gp,0(sp)     | WB sw ra,0(sp)
cyc  8 | IF 0x00000018 | ID sw tp,4(sp)    | EX add tp,gp,gp   | MEM -               | WB lw gp,0(sp)
```

The `-r` flag additionally dumps all 32 registers (with ABI names) and the
final `pc` when the simulation ends.

## Pipeline model

The pipeline is in-order and single-issue:

- **IF (Instruction Fetch)** reads the next instruction word from memory.
- **ID (Instruction Decode)** decodes it, reads source registers, and
  detects load-use hazards.
- **EX (Execute)** performs ALU and M-extension operations, computes
  addresses, and resolves branches and jumps.
- **MEM (Memory)** performs loads and stores.
- **WB (Write Back)** commits results to the register file and handles
  system instructions.

Hazards are handled the way a textbook five-stage machine handles them:

| Hazard | Resolution | Cost |
| --- | --- | --- |
| ALU result needed by the next instruction | EX/MEM -> EX forwarding | none |
| Result needed two instructions later | MEM/WB -> EX forwarding | none |
| Load result needed by the next instruction | One-cycle interlock, then forwarding | 1 stall cycle |
| `lw` immediately followed by `sw` of the same register | MEM/WB -> MEM store-data forwarding | none |
| Taken branch or jump | Resolved in EX (static predict-not-taken); the wrong-path instructions in IF and ID are squashed | 2 squashed instructions |

## Supported instructions

| Category | Instructions |
| --- | --- |
| Upper immediate | `lui`, `auipc` |
| Jumps | `jal`, `jalr` |
| Branches | `beq`, `bne`, `blt`, `bge`, `bltu`, `bgeu` |
| Loads | `lb`, `lh`, `lw`, `lbu`, `lhu` |
| Stores | `sb`, `sh`, `sw` |
| Immediate ALU | `addi`, `slti`, `sltiu`, `xori`, `ori`, `andi`, `slli`, `srli`, `srai` |
| Register ALU | `add`, `sub`, `sll`, `slt`, `sltu`, `xor`, `srl`, `sra`, `or`, `and` |
| RV32M | `mul`, `mulh`, `mulhsu`, `mulhu`, `div`, `divu`, `rem`, `remu` |
| System | `fence`, `ecall`, `ebreak` |

`fence` is a no-op (there is nothing to order in a single flat memory).
`ecall` invokes the simulator's syscall interface below, and `ebreak` halts
execution.

## Command line

```text
usage: ./rvsim [options] [program.hex|program.bin]
  -t            trace pipeline occupancy every cycle (stderr)
  -r            dump registers when the simulation ends
  -c <cycles>   cycle budget (default 10000000)
  -m <bytes>    memory size (default 1 MiB)
```

## Writing programs

Programs are plain text files of whitespace-separated 32-bit instruction
words in hex, loaded sequentially starting at address zero (which is also
the reset `pc`). Both `#` and `//` start line comments, so programs can be
annotated like a listing:

```text
00500093   # addi ra, zero, 5
00008513   # addi a0, ra, 0
00100893   # addi a7, zero, 1    syscall 1: print integer
00000073   # ecall
05d00893   # addi a7, zero, 93   syscall 93: exit
00000073   # ecall
```

Files ending in `.bin` are instead loaded as raw little-endian binaries,
also at address zero — so output from a real assembler (e.g.
`riscv64-unknown-elf-objcopy -O binary`) works too.

### Syscalls

`ecall` reads the syscall number from `a7` and the argument from `a0`:

| `a7` | Operation |
| --- | --- |
| `1` | Print `a0` as a signed decimal integer followed by a newline |
| `2` | Print the low byte of `a0` as an ASCII character |
| `3` | Print the NUL-terminated string at memory address `a0` |
| `10`, `93` | Exit with the low byte of `a0` as the status code |

This is a small testing convenience, not the Linux ABI — but `93` matches
the real Linux `exit` number, so simple assembler programs carry over.

## Compiling and running a C program

The `cdemo/` directory shows that the simulator can run code produced by a
real C compiler, rather than only hand-written hex files. The demo sorts ten
integers, prints them, computes `gcd(84, 30)`, and exits successfully.

It is important to distinguish this from running an ordinary Linux program.
When a program runs on Linux, several layers do work before `main` begins:

- the executable loader maps an ELF file into memory;
- startup code initializes registers and calls `main`;
- the C library provides functions such as `printf` and `memcpy`;
- the operating system implements files, processes, and system calls.

This simulator intentionally provides none of those layers. It models a CPU, flat
memory, and a few simple output syscalls. The demo is therefore a
**freestanding C program**: normal C control flow and data structures work,
but the small amount of machine-level setup normally hidden by an OS and C
library is included explicitly in `cdemo/`.

### Demo files and why they exist

| File | Purpose |
| --- | --- |
| [`cdemo/demo.c`](cdemo/demo.c) | The application: sorting, GCD, strings, stack variables, and wrappers around the simulator's syscalls |
| [`cdemo/start.S`](cdemo/start.S) | Supplies the `_start` entry point, initializes the stack pointer, calls `main`, and exits with its return value |
| [`cdemo/runtime.c`](cdemo/runtime.c) | Supplies `memcpy` and `memset`, since there is no C library to provide compiler helper routines |
| [`cdemo/linker.ld`](cdemo/linker.ld) | Assigns final memory addresses, places `_start` at address zero, and defines the top of the stack |
| [`cdemo/Makefile`](cdemo/Makefile) | Runs the cross-compilation, linking, binary extraction, and simulator commands reproducibly |

Each supporting file contains comments explaining the less familiar parts.
Generated objects, `demo.elf`, and `demo.bin` are ignored by the repository's
root `.gitignore` because they can always be rebuilt.

### Prerequisites

The demo uses:

- Clang with its RISC-V backend;
- GNU RISC-V binutils (`riscv64-linux-gnu-ld`, `objcopy`, and `objdump`);
- Make.

Build and run everything from the repository root with:

```sh
make -C cdemo run
```

The program prints:

```text
sorted:
-8
-3
0
1
5
7
9
15
23
42
gcd(84, 30):
6
```

### From source code to simulated instructions

The complete path looks like this:

```text
demo.c + runtime.c + start.S
             |
             |  Clang compiles each source file
             v
       demo.o + runtime.o + start.o
             |
             |  GNU ld applies linker.ld
             v
          demo.elf
             |
             |  objcopy extracts loadable bytes
             v
          demo.bin
             |
             |  rvsim loads it at address 0
             v
       simulated RV32IM execution
```

Here is what each step means.

1. **Clang translates source into object files.**

   `demo.c` and `runtime.c` are translated from C into RV32IM machine
   instructions. `start.S` is assembled into instructions as well. The `.o`
   files contain code and data, but their final addresses are not known yet,
   so they cannot be loaded directly into the simulator.

2. **The compiler flags describe the simulated machine.**

   | Flag | Reason |
   | --- | --- |
   | `--target=riscv32-none-elf` | Generate bare-metal RISC-V code instead of code for the host computer |
   | `-march=rv32im` | Use only the 32-bit integer and multiply/divide instructions implemented by `rvsim` |
   | `-mabi=ilp32` | Use 32-bit integers, pointers, registers, and the standard RISC-V calling convention |
   | `-ffreestanding` | Tell Clang that there is no hosted OS or complete standard library |
   | `-fno-builtin` | Prevent ordinary C calls from being silently replaced with unavailable library builtins |
   | `-fno-stack-protector` | Avoid references to stack-protection runtime functions that are not present |
   | `-fno-pic -fno-pie` | Generate a fixed-address image for the simulator's flat memory |
   | `-mno-relax` | Keep the assembler/linker from rewriting instruction sequences into forms that depend on extra runtime setup |
   | `-msmall-data-limit=0` | Avoid global-pointer-relative data accesses, so startup only needs to initialize `sp` |
   | `-O2` | Optimize the C program while still producing ordinary RV32IM instructions |

   The Makefile invokes the linker directly, so no host startup files or libc
   are linked accidentally.

3. **`runtime.c` supplies compiler support that libc normally provides.**

   The array in `main` has ten initial values. In the current optimized build,
   Clang stores those 40 constant bytes in the read-only-data section and
   emits a call to `memcpy` to copy them into `main`'s stack frame. Without
   `runtime.c`, the link fails with an undefined `memcpy` reference.

   This can happen even in freestanding mode: freestanding tells the compiler
   that a complete C library is unavailable, but generated code may still need
   fundamental memory helpers. `memset` is included for the same reason even
   though this particular build currently uses only `memcpy`.

4. **The linker turns separate pieces into one addressed program.**

   `riscv64-linux-gnu-ld` combines the three object files according to
   `linker.ld`. The script lays out:

   - `.text`, containing executable instructions, starting at address `0`;
   - `.rodata`, containing string literals and the initial array values;
   - `.data`, for writable initialized global data;
   - `.bss`, for zero-initialized global data;
   - `__stack_top` at `0x00100000`, the end of the default 1 MiB memory.

   The result is `demo.elf`. ELF is a structured executable format containing
   section addresses, symbols, permissions, and other metadata. It is useful
   to linkers, debuggers, and disassemblers, but `rvsim` does not currently
   parse that structure.

5. **`objcopy` creates the flat image understood by the simulator.**

   `riscv64-linux-gnu-objcopy -O binary` extracts the loadable code and data
   bytes from `demo.elf` into `demo.bin`. The raw file has no section names or
   symbols; it is just the bytes that need to appear in simulated memory.

6. **`rvsim` loads the bytes and begins at `_start`.**

   Because the input filename ends in `.bin`, the simulator copies the file's
   bytes into its zero-initialized memory beginning at address `0`. The reset
   program counter is also `0`, and the linker deliberately placed `_start`
   there, so the first fetched instruction is startup code rather than `main`.

7. **`start.S` creates the environment expected by compiled C.**

   The startup instructions set `sp` to `0x00100000`. The stack grows toward
   lower addresses, so the first function prologue subtracts space before
   writing anything. The address is 16-byte aligned as required by the RISC-V
   calling convention. `_start` then calls `main`, which also puts the return
   address in `ra` so execution can come back afterward.

   The simulator initializes memory to zero, so this demo does not need the
   `.bss` clearing loop that startup code would usually perform on physical
   bare-metal hardware.

8. **The compiler-generated `main` runs through the pipeline.**

   In the current build, `main` reserves 48 bytes on the stack, copies the
   40-byte initial array there, and calls the sorting function. All of the
   generated loads, stores, comparisons, branches, calls, and returns are
   ordinary instructions flowing through IF, ID, EX, MEM, and WB. The GCD
   calculation uses an M-extension remainder instruction.

9. **Output crosses the CPU/simulator boundary with `ecall`.**

   The wrappers in `demo.c` put an argument in `a0` and a syscall number in
   `a7`. An `ecall` then travels through the pipeline like another instruction.
   When it reaches WB, `rvsim` handles it: syscall `1` prints an integer and
   syscall `3` follows a simulated-memory address and prints a string.

10. **Returning from `main` stops the simulation.**

    RISC-V convention places `main`'s return value in `a0`. After `main`
    returns, `_start` puts `93` in `a7` and executes one final `ecall`. The
    simulator records the low byte of `a0` as the exit status, halts, and
    prints its cycle, CPI, stall, and branch statistics.

Use the following command to inspect the exact instructions emitted by your
installed compiler:

```sh
make -C cdemo disassemble
```

Compiler versions and optimization choices can change the exact instruction
sequence and cycle count while preserving the program's output.

## Tests

`make test` builds the simulator and runs every bundled program:

| Program | Exercises | Expected output |
| --- | --- | --- |
| `tests/sum.hex` | Looping and backward branches | `55` |
| `tests/hazards.hex` | Forwarding, load-use stall, `lw`;`sw` forwarding | `84` |
| `tests/jump.hex` | Jump redirects and wrong-path squashing | `5` |
| `tests/sort.hex` | Bubble sort: nested loops plus memory traffic | `-8 -3 0 1 5 7 9 15 23 42`, one per line |

Each test is self-checking in the loose sense that its output is easy to
verify by eye; the statistics on standard error (cycles, instructions
retired, CPI, stalls, redirects, squashes) show *how* it ran.

## Project layout

```text
rv-five-stage.cpp  The entire simulator: decode, pipeline, memory, syscalls, CLI
tests/             Annotated test programs (.hex)
cdemo/             Freestanding C demo and its startup/linker support
Makefile           Build (`make`) and test (`make test`) targets
compile_flags.txt  Project flags used by clangd
```

## Limitations

Deliberately out of scope, to keep the model small and readable:

- No caches, MMU, interrupts, privileged modes, or trap handlers.
- No compressed (C) or floating-point (F/D) instructions.
- Memory is a fixed-size flat byte array; misaligned instruction fetches
  and data accesses are fatal simulation errors rather than traps.
- Branch prediction is always not-taken; there is no BTB or history.
