#!/usr/bin/env python3
"""Random memory-soup generator for differential testing.

Emits a straight-line RV32IM program (rvsim .hex format) full of loads
and stores confined to a few cache lines (maximum aliasing pressure
for the load/store queue), mixed with ALU ops, multiplies, divides,
and forward branches. The functional reference model is the oracle
(run the output under -d), so this script needs no knowledge of what
the program computes; it only guarantees alignment and convergent
control flow. Usage: randgen.py SEED [NOPS]
"""
import random
import sys

seed = int(sys.argv[1])
nops = int(sys.argv[2]) if len(sys.argv) > 2 else 60
random.seed(seed)

out = []


def emit(word, txt):
    out.append(f"{word:08X}   # {txt}")


def i_t(imm, rs1, f3, rd, op):
    return ((imm & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op


def r_t(f7, rs2, rs1, f3, rd):
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | 0x33


def s_t(imm, rs2, rs1, f3):
    return (((imm >> 5) & 0x7F) << 25) | (rs2 << 20) | (rs1 << 15) \
        | (f3 << 12) | ((imm & 0x1F) << 7) | 0x23


def b_t(imm, rs2, rs1, f3):
    u = imm & 0x1FFF
    return (((u >> 12) & 1) << 31) | (((u >> 5) & 0x3F) << 25) | (rs2 << 20) \
        | (rs1 << 15) | (f3 << 12) | (((u >> 1) & 0xF) << 8) \
        | (((u >> 11) & 1) << 7) | 0x63


# Three lines to fight over, parked at 8 KiB; that is far above any
# program this generator can emit, so stores can never reach the code
DATA_REGION = 0x2000
BASES = [(1, 0x00), (2, 0x40), (3, 0x80)]
DATA = [4, 5, 6, 7, 8, 9, 11, 12, 13, 14]  # scratch registers

for reg, off in BASES:
    emit((2 << 12) | (reg << 7) | 0x37, f"lui x{reg}, 0x2")  # 0x2000
    if off:
        emit(i_t(off, reg, 0, reg, 0x13), f"addi x{reg}, x{reg}, {off}")
for k, reg in enumerate(DATA):
    v = random.randint(1, 200)
    emit(i_t(v, 0, 0, reg, 0x13), f"addi x{reg}, x0, {v}")


def alu():
    rd, a, b = random.choice(DATA), random.choice(DATA), random.choice(DATA)
    kind = random.random()
    if kind < 0.5:
        v = random.randint(-100, 100)
        emit(i_t(v, a, 0, rd, 0x13), f"addi x{rd}, x{a}, {v}")
    elif kind < 0.7:
        emit(r_t(0, b, a, 4, rd), f"xor x{rd}, x{a}, x{b}")
    elif kind < 0.85:
        emit(r_t(1, b, a, 0, rd), f"mul x{rd}, x{a}, x{b}")
    else:
        emit(r_t(1, b, a, 5, rd), f"divu x{rd}, x{a}, x{b}")


def mem(is_store):
    base = random.choice(BASES)[0]
    reg = random.choice(DATA)
    width = random.choices([4, 1, 2], weights=[6, 2, 2])[0]
    off = random.randrange(0, 64, width)
    if is_store:
        f3 = {1: 0, 2: 1, 4: 2}[width]
        name = {1: "sb", 2: "sh", 4: "sw"}[width]
        emit(s_t(off, reg, base, f3), f"{name} x{reg}, {off}(x{base})")
    else:
        f3 = random.choice({1: [0, 4], 2: [1, 5], 4: [2]}[width])
        name = {0: "lb", 1: "lh", 2: "lw", 4: "lbu", 5: "lhu"}[f3]
        emit(i_t(off, base, f3, reg, 0x03), f"{name} x{reg}, {off}(x{base})")


def conflict_store():
    # 0x3000..0xb000 all map to the same set as the 0x2000 base line:
    # nine lines fighting over eight ways, so the dirty base line gets
    # evicted regularly and promptly re-accessed: real pressure on the
    # writeback queue and its ordering against refills
    k = random.randint(3, 11)
    reg = random.choice(DATA)
    emit((k << 12) | (15 << 7) | 0x37, f"lui x15, {k:#x}")
    emit(s_t(0, reg, 15, 2), f"sw x{reg}, 0(x15)")


def slow_mem(is_store):
    # An address that resolves LATE: derived from a divide, masked back
    # into a base line. This is what forces load speculation (and
    # replays, when a younger load to the same line guessed wrong)
    t = 15  # dedicated address-scratch register
    a, b = random.choice(DATA), random.choice(DATA)
    base = random.choice(BASES)[0]
    reg = random.choice(DATA)
    emit(r_t(1, b, a, 5, t), f"divu x{t}, x{a}, x{b}")
    emit(i_t(60, t, 7, t, 0x13), f"andi x{t}, x{t}, 60")
    emit(r_t(0, base, t, 0, t), f"add x{t}, x{t}, x{base}")
    if is_store:
        emit(s_t(0, reg, t, 2), f"sw x{reg}, 0(x{t})")
    else:
        emit(i_t(0, t, 2, reg, 0x03), f"lw x{reg}, 0(x{t})")


n = 0
while n < nops:
    r = random.random()
    if r < 0.25:
        alu()
    elif r < 0.45:
        mem(is_store=True)
    elif r < 0.67:
        mem(is_store=False)
    elif r < 0.76:  # late-resolving address: the speculation stressor
        slow_mem(is_store=random.random() < 0.7)
        n += 3
    elif r < 0.90:  # eviction pressure: the writeback-queue stressor
        conflict_store()
        n += 1
    else:  # forward branch over one instruction; both paths rejoin,
           # so the program is valid whichever way it goes
        a, b = random.choice(DATA), random.choice(DATA)
        f3, name = random.choice([(0, "beq"), (1, "bne")])
        emit(b_t(8, b, a, f3), f"{name} x{a}, x{b}, +8")
        alu()
        n += 1
    n += 1

# checksum: fold every scratch register into a0 and print it
emit(i_t(0, DATA[0], 0, 10, 0x13), f"addi x10, x{DATA[0]}, 0")
for reg in DATA[1:]:
    emit(r_t(0, reg, 10, 4, 10), f"xor x10, x10, x{reg}")
emit(i_t(1, 0, 0, 17, 0x13), "addi x17, x0, 1")
emit(0x73, "ecall")
emit(i_t(0, 0, 0, 10, 0x13), "addi x10, x0, 0")
emit(i_t(93, 0, 0, 17, 0x13), "addi x17, x0, 93")
emit(0x73, "ecall")

if 4 * len(out) >= DATA_REGION:
    sys.exit(f"randgen: {len(out)} instructions would overlap the "
             f"data region at {DATA_REGION:#x}; lower NOPS")

print(f"# random memory soup, seed {seed}, {nops} ops (tests/randgen.py)")
print("\n".join(out))
