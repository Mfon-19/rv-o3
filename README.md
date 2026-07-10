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
Makefile           Build (`make`) and test (`make test`) targets
```

## Limitations

Deliberately out of scope, to keep the model small and readable:

- No caches, MMU, interrupts, privileged modes, or trap handlers.
- No compressed (C) or floating-point (F/D) instructions.
- Memory is a fixed-size flat byte array; misaligned instruction fetches
  and data accesses are fatal simulation errors rather than traps.
- Branch prediction is always not-taken; there is no BTB or history.
