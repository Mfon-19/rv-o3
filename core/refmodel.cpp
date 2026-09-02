#include "core/refmodel.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "isa/execute.h"
#include "sim/syscall.h"

RefModel::RefModel(const SimConfig &cfg)
    : mem(cfg.memBytes), maxInstrs(cfg.maxCycles) {
  memset(regs, 0, sizeof regs);
}

bool RefModel::step(CommitRecord *rec) {
  if (halted_)
    return false;

  if (pc % 4 != 0) {
    fprintf(stderr, "fatal: misaligned fetch at pc=0x%8x\n", pc);
    exit(1);
  }
  const uint32_t raw = mem.load32(pc);
  const Instr I = decode(raw);

  CommitRecord r;
  r.sequence = retired_;
  r.pc = pc;
  r.instruction = raw;

  uint32_t nextPC = pc + 4;

  switch (I.op) {
  case Op::ECALL:
    retired_++;
    if (runSyscall(
            regs[17], regs[10], pc, quiet,
            [&](uint32_t a) { return mem.load8(a); }, exitCode_))
      halted_ = true;
    break;
  case Op::EBREAK:
    retired_++;
    if (!quiet)
      fprintf(stderr, "ebreak at pc=0x%08x; halting\n", pc);
    halted_ = true;
    break;
  case Op::ILLEGAL:
    if (!quiet)
      fprintf(stderr, "illegal instruction 0x%08x at pc=0x%08x\n", raw, pc);
    halted_ = true;
    exitCode_ = 1;
    r.exception = Exception{ExceptionKind::IllegalInstruction};
    break;
  default: {
    const uint32_t a = usesRs1(I.op) ? regs[I.rs1] : 0;
    const uint32_t b = usesRs2(I.op) ? regs[I.rs2] : 0;
    const ExecResult x = execute(I, pc, a, b);
    uint32_t result = x.value;

    if (isLoad(I.op) || isStore(I.op)) {
      const uint32_t addr = x.value, size = accessSize(I.op);
      if (addr % size != 0)
        Memory::failMisaligned(addr, isStore(I.op) ? "store" : "load", pc);
      if (isLoad(I.op)) {
        result = extendLoad(I.op, mem.load(addr, size));
      } else {
        mem.store(addr, size, b);
        r.memoryWrite = MemoryWrite{addr, b, (uint8_t)size};
      }
    }

    if (writesRd(I.op) && I.rd != 0) {
      regs[I.rd] = result;
      r.registerWrite = RegisterWrite{I.rd, result};
    }
    retired_++;
    nextPC = x.redirect ? x.target : pc + 4;
    break;
  }
  }

  if (!halted_)
    pc = nextPC;

  if (rec)
    *rec = r;
  return true;
}

int RefModel::run() {
  while (!halted_) {
    if (retired_ >= maxInstrs) {
      fprintf(stderr,
              "stopping after %" PRIu64
              " instructions without an exit syscall (raise with -c)\n",
              retired_);
      exitCode_ = 2;
      break;
    }
    step();
  }
  fprintf(stderr,
          "--- rvsim: functional model, %" PRIu64 " instructions retired\n",
          retired_);
  fprintf(stderr, "--- rvsim: exit code %d\n", exitCode_);
  return exitCode_;
}
