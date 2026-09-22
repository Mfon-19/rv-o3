#include "core/refmodel.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "isa/execute.h"
#include "sim/syscall.h"

RefModel::RefModel(const SimConfig &cfg) : mem(cfg.memBytes) {
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
            regs[17], regs[10], pc, true,
            [&](uint32_t a) { return mem.load8(a); }, exitCode_, replayInput))
      halted_ = true;
    if (regs[17] == 5)
      r.registerWrite = RegisterWrite{10, regs[10]};
    break;
  case Op::EBREAK:
    retired_++;
    halted_ = true;
    break;
  case Op::ILLEGAL:
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
