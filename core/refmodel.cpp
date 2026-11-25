// Single-instruction functional step of the golden RV32IM reference model.

#include "core/refmodel.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sim/syscall.h"

RefModel::RefModel(const SimConfig &config) : mem(config.memBytes) {
  memset(regs, 0, sizeof regs);
}

bool RefModel::step(CommitRecord *outRecord) {
  if (halted_)
    return false;

  if (pc % 4 != 0) {
    fprintf(stderr, "fatal: misaligned fetch at pc=0x%8x\n", pc);
    exit(1);
  }

  const uint32_t rawWord = mem.load32(pc);
  const Instr instr = decode(rawWord);

  CommitRecord record;
  record.sequence = retired_;
  record.pc = pc;
  record.instruction = rawWord;

  uint32_t nextPc = pc + 4;

  switch (instr.op) {
  case Op::ECALL:
    retired_++;
    // Run in quiet mode so host output/frames are not duplicated under `-d`.
    if (runSyscall(
            regs[17], regs[10], pc, true,
            [&](uint32_t addr) { return mem.load8(addr); }, exitCode_,
            mem.bytes.size(), replayInput)) {
      halted_ = true;
    }

    if (syscallReturnsInput(regs[17]))
      record.registerWrite = RegisterWrite{10, regs[10]};
    break;

  case Op::EBREAK:
    retired_++;
    halted_ = true;
    break;

  case Op::ILLEGAL:
    halted_ = true;
    exitCode_ = 1;
    record.exception = Exception{ExceptionKind::IllegalInstruction};
    break;

  default: {
    const uint32_t src1 = usesRs1(instr.op) ? regs[instr.rs1] : 0;
    const uint32_t src2 = usesRs2(instr.op) ? regs[instr.rs2] : 0;
    const ExecResult execResult = execute(instr, pc, src1, src2);
    uint32_t result = execResult.value;

    if (isLoad(instr.op) || isStore(instr.op)) {
      const uint32_t addr = execResult.value;
      const uint32_t size = accessSize(instr.op);

      if (addr % size != 0)
        Memory::failMisaligned(addr, isStore(instr.op) ? "store" : "load", pc);

      if (isLoad(instr.op)) {
        result = extendLoad(instr.op, mem.load(addr, size));
      } else {
        mem.store(addr, size, src2);
        record.memoryWrite = MemoryWrite{addr, src2, (uint8_t)size};
      }
    }

    if (writesRd(instr.op) && instr.rd != 0) {
      regs[instr.rd] = result;
      record.registerWrite = RegisterWrite{instr.rd, result};
    }

    retired_++;
    nextPc = execResult.redirect ? execResult.target : pc + 4;
    break;
  }
  }

  if (!halted_)
    pc = nextPc;

  if (outRecord)
    *outRecord = record;

  return true;
}
