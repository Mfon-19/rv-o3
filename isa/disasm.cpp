// Disassembler (used only for the -t trace and error messages)

#include "isa/isa.h"

#include <cstdio>

const char *const kRegName[32] = {
    "zero", "ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "s0", "s1", "a0",
    "a1",   "a2", "a3", "a4", "a5",  "a6",  "a7", "s2", "s3", "s4", "s5",
    "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

std::string disasm(const Instr &I) {
  char b[64];
  const char *n = opName(I.op);
  const char *rd = kRegName[I.rd];
  const char *r1 = kRegName[I.rs1];
  const char *r2 = kRegName[I.rs2];
  switch (I.op) {
  case Op::LUI:
  case Op::AUIPC:
    snprintf(b, sizeof b, "%s %s,0x%x", n, rd, (uint32_t)I.imm >> 12);
    break;
  case Op::JAL:
    snprintf(b, sizeof b, "%s %s,%d", n, rd, I.imm);
    break;
  case Op::JALR:
    snprintf(b, sizeof b, "%s %s,%d(%s)", n, rd, I.imm, r1);
    break;
  case Op::BEQ:
  case Op::BNE:
  case Op::BLT:
  case Op::BGE:
  case Op::BLTU:
  case Op::BGEU:
    snprintf(b, sizeof b, "%s %s,%s,%d", n, r1, r2, I.imm);
    break;
  case Op::LB:
  case Op::LH:
  case Op::LW:
  case Op::LBU:
  case Op::LHU:
    snprintf(b, sizeof b, "%s %s,%d(%s)", n, rd, I.imm, r1);
    break;
  case Op::SB:
  case Op::SH:
  case Op::SW:
    snprintf(b, sizeof b, "%s %s,%d(%s)", n, r2, I.imm, r1);
    break;
  case Op::ADDI:
  case Op::SLTI:
  case Op::SLTIU:
  case Op::XORI:
  case Op::ORI:
  case Op::ANDI:
  case Op::SLLI:
  case Op::SRLI:
  case Op::SRAI:
    snprintf(b, sizeof b, "%s %s,%s,%d", n, rd, r1, I.imm);
    break;
  case Op::FENCE:
  case Op::ECALL:
  case Op::EBREAK:
    snprintf(b, sizeof b, "%s", n);
    break;
  case Op::ILLEGAL:
    snprintf(b, sizeof b, ".word 0x%08x", I.raw);
    break;
  default: // all remaining R-type ops share one format
    snprintf(b, sizeof b, "%s %s,%s,%s", n, rd, r1, r2);
    break;
  }
  return b;
}
