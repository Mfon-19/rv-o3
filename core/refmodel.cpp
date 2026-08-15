#include "core/refmodel.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "isa/execute.h"

RefModel::RefModel(const SimConfig &cfg)
    : mem(cfg.memBytes), maxInstrs(cfg.maxCycles) {
  memset(regs, 0, sizeof regs);
}

void RefModel::loadWords(const std::vector<uint32_t> &words) {
  for (size_t i = 0; i < words.size(); i++) {
    mem.store32((uint32_t)(i * 4), words[i]);
  }
}

void RefModel::loadBytes(const std::vector<uint8_t> &bytes) {
  mem.check(0, (uint32_t)bytes.size(), "program image");
  memcpy(mem.bytes.data(), bytes.data(), bytes.size());
}

// Same alignment policy as the pipeline: RV32I permits trapping on
// misaligned data accesses, but we treat them as fatal since we don't
// model trap handling
void RefModel::checkAlign(Op op, uint32_t addr) const {
  uint32_t need = 1;
  if (op == Op::LH || op == Op::LHU || op == Op::SH)
    need = 2;
  if (op == Op::LW || op == Op::SW)
    need = 4;
  if (addr % need != 0) {
    fprintf(stderr, "fatal: misaligned %s of 0x%08x at pc=0x%08x\n",
            isStore(op) ? "store" : "load", addr, pc);
    exit(1);
  }
}

bool RefModel::step(CommitRecord *rec) {
  if (halted_)
    return false;

  // fetch
  if (pc % 4 != 0) {
    fprintf(stderr, "fatal: misaligned fetch at pc=0x%8x\n", pc);
    exit(1);
  }
  const uint32_t raw = mem.load32(pc);

  // decode
  const Instr I = decode(raw);

  CommitRecord r;
  r.sequence = retired_;
  r.pc = pc;
  r.instruction = raw;

  uint32_t nextPC = pc + 4;

  // execute + commit
  switch (I.op) {
  case Op::ECALL:
    retired_++;
    doSyscall();
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

    if (isLoad(I.op)) {
      const uint32_t addr = x.value;
      checkAlign(I.op, addr);
      switch (I.op) {
      // Sub-word loads sign- or zero-extended into 32 bits as per the ISA
      case Op::LB:
        result = (uint32_t)(int32_t)(int8_t)mem.load8(addr);
        break;
      case Op::LBU:
        result = mem.load8(addr);
        break;
      case Op::LH:
        result = (uint32_t)(int32_t)(int16_t)mem.load16(addr);
        break;
      case Op::LHU:
        result = mem.load16(addr);
        break;
      case Op::LW:
        result = mem.load32(addr);
        break;
      default:
        break;
      }
    } else if (isStore(I.op)) {
      const uint32_t addr = x.value;
      checkAlign(I.op, addr);
      switch (I.op) {
      case Op::SB:
        mem.store8(addr, (uint8_t)b);
        r.memoryWrite = MemoryWrite{addr, (uint8_t)b, 1};
        break;
      case Op::SH:
        mem.store16(addr, (uint16_t)b);
        r.memoryWrite = MemoryWrite{addr, (uint16_t)b, 2};
        break;
      case Op::SW:
        mem.store32(addr, b);
        r.memoryWrite = MemoryWrite{addr, b, 4};
        break;
      default:
        break;
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

// Minimal syscall interface, identical to the pipeline's
// (a7 = number, a0 = argument)
//   1  print a0 as a signed decimal integer, followed by a newline
//   2  print a0 as a single ASCII character
//   3  print the NUL-terminated string at address a0
//   93 exit with code a0  (Linux-flavoured number; 10 also accepted)
void RefModel::doSyscall() {
  const uint32_t num = regs[17]; // a7
  const uint32_t arg = regs[10]; // a0
  switch (num) {
  case 1:
    if (!quiet)
      printf("%d\n", (int32_t)arg);
    break;
  case 2:
    if (!quiet)
      putchar((int)(arg & 0xFF));
    break;
  case 3:
    if (!quiet)
      for (uint32_t a = arg; mem.load8(a) != 0; a++)
        putchar(mem.load8(a));
    break;
  case 4: // print a0 as 8 hex digits (architectural-test signatures)
    if (!quiet)
      printf("%08x\n", arg);
    break;
  case 10:
  case 93:
    halted_ = true;
    exitCode_ = (int)(arg & 0xFF);
    break;
  default:
    if (!quiet)
      fprintf(stderr, "warning: unknown syscall a7=%u at pc=0x%08x\n", num, pc);
    break;
  }
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

void RefModel::dumpRegs() const {
  fprintf(stderr, "--- registers ---\n");
  for (int i = 0; i < 32; i++) {
    fprintf(stderr, "%4s=%08x%s", kRegName[i], regs[i],
            (i % 4 == 3) ? "\n" : "  ");
  }
  fprintf(stderr, "pc  =%08x\n", pc);
}
