// RV32IM instruction decoder, functional ALU/branch/M-extension evaluator,
// register dumper, and disassembler.

#include "isa/isa.h"

#include <cstdio>

const char *const kRegName[32] = {
    "zero", "ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "s0", "s1", "a0",
    "a1",   "a2", "a3", "a4", "a5",  "a6",  "a7", "s2", "s3", "s4", "s5",
    "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

namespace {

// Immediate extraction helpers for each RISC-V instruction encoding format.
int32_t immI(uint32_t word) { return (int32_t)word >> 20; }
int32_t immU(uint32_t word) { return (int32_t)(word & 0xFFFFF000u); }

int32_t immS(uint32_t word) {
  return ((int32_t)(word & 0xFE000000u) >> 20) | (int32_t)((word >> 7) & 0x1F);
}

int32_t immB(uint32_t word) {
  return ((int32_t)(word & 0x80000000u) >> 19) |
         (int32_t)((word & 0x00000080u) << 4) |
         (int32_t)((word >> 20) & 0x7E0) | (int32_t)((word >> 7) & 0x1E);
}

int32_t immJ(uint32_t word) {
  return ((int32_t)(word & 0x80000000u) >> 11) | (int32_t)(word & 0x000FF000u) |
         (int32_t)((word >> 9) & 0x800) | (int32_t)((word >> 20) & 0x7FE);
}

// funct3 lookup tables for each major opcode group.
constexpr Op kBranchOps[8] = {
    Op::BEQ, Op::BNE, Op::ILLEGAL, Op::ILLEGAL,
    Op::BLT, Op::BGE, Op::BLTU,    Op::BGEU,
};
constexpr Op kLoadOps[8] = {
    Op::LB, Op::LH, Op::LW, Op::ILLEGAL, Op::LBU, Op::LHU, Op::ILLEGAL, Op::ILLEGAL,
};
constexpr Op kStoreOps[8] = {
    Op::SB, Op::SH, Op::SW, Op::ILLEGAL, Op::ILLEGAL, Op::ILLEGAL, Op::ILLEGAL, Op::ILLEGAL,
};
constexpr Op kImmAluOps[8] = {
    Op::ADDI, Op::SLLI, Op::SLTI, Op::SLTIU, Op::XORI, Op::SRLI, Op::ORI, Op::ANDI,
};
constexpr Op kRegAluOps[8] = {
    Op::ADD, Op::SLL, Op::SLT, Op::SLTU, Op::XOR, Op::SRL, Op::OR, Op::AND,
};
constexpr Op kRegAluAltOps[8] = {
    Op::SUB, Op::ILLEGAL, Op::ILLEGAL, Op::ILLEGAL, Op::ILLEGAL, Op::SRA, Op::ILLEGAL, Op::ILLEGAL,
};
constexpr Op kMulDivOps[8] = {
    Op::MUL, Op::MULH, Op::MULHSU, Op::MULHU, Op::DIV, Op::DIVU, Op::REM, Op::REMU,
};

}

Instr decode(uint32_t word) {
  Instr instr;
  instr.raw = word;
  instr.rd = (word >> 7) & 31;
  instr.rs1 = (word >> 15) & 31;
  instr.rs2 = (word >> 20) & 31;

  const uint32_t funct3 = (word >> 12) & 7;
  const uint32_t funct7 = word >> 25;

  switch (word & 0x7F) {
  case 0x37: instr.op = Op::LUI; instr.imm = immU(word); break;
  case 0x17: instr.op = Op::AUIPC; instr.imm = immU(word); break;
  case 0x6F: instr.op = Op::JAL; instr.imm = immJ(word); break;
  case 0x63: instr.op = kBranchOps[funct3]; instr.imm = immB(word); break;
  case 0x03: instr.op = kLoadOps[funct3]; instr.imm = immI(word); break;
  case 0x23: instr.op = kStoreOps[funct3]; instr.imm = immS(word); break;
  case 0x0F: instr.op = Op::FENCE; break;

  case 0x67:
    if (funct3 == 0) {
      instr.op = Op::JALR;
      instr.imm = immI(word);
    }
    break;

  case 0x13:
    instr.op = kImmAluOps[funct3];
    instr.imm = immI(word);

    // Shift-immediate instructions encode the shift amount in rs2 and require
    // funct7 to be 0x00 (or 0x20 for arithmetic right shift).
    if (funct3 == 1 || funct3 == 5) {
      if (funct3 == 5 && funct7 == 0x20)
        instr.op = Op::SRAI;
      else if (funct7 != 0x00)
        instr.op = Op::ILLEGAL;

      if (instr.op != Op::ILLEGAL)
        instr.imm = instr.rs2;
    }
    break;

  case 0x33:
    if (funct7 == 0x00) instr.op = kRegAluOps[funct3];
    else if (funct7 == 0x20) instr.op = kRegAluAltOps[funct3];
    else if (funct7 == 0x01) instr.op = kMulDivOps[funct3];
    break;

  case 0x73:
    if (word == 0x00000073) instr.op = Op::ECALL;
    if (word == 0x00100073) instr.op = Op::EBREAK;
    break;
  }

  return instr;
}

ExecResult execute(const Instr &instr, uint32_t pc, uint32_t src1, uint32_t src2) {
  ExecResult result;
  const int32_t signedSrc1 = (int32_t)src1;
  const int32_t signedSrc2 = (int32_t)src2;
  const uint32_t imm = (uint32_t)instr.imm;

  switch (instr.op) {
  case Op::LUI:    result.value = imm; break;
  case Op::AUIPC:  result.value = pc + imm; break;
  case Op::JAL:    result = {pc + 4, true, pc + imm}; break;
  case Op::JALR:   result = {pc + 4, true, (src1 + imm) & ~1u}; break;

  case Op::BEQ:    result.redirect = src1 == src2; break;
  case Op::BNE:    result.redirect = src1 != src2; break;
  case Op::BLT:    result.redirect = signedSrc1 < signedSrc2; break;
  case Op::BGE:    result.redirect = signedSrc1 >= signedSrc2; break;
  case Op::BLTU:   result.redirect = src1 < src2; break;
  case Op::BGEU:   result.redirect = src1 >= src2; break;

  // Loads and stores compute their effective address using rs1 + imm.
  case Op::LB: case Op::LH: case Op::LW: case Op::LBU: case Op::LHU:
  case Op::SB: case Op::SH: case Op::SW: case Op::ADDI:
    result.value = src1 + imm;
    break;

  case Op::SLTI:   result.value = signedSrc1 < (int32_t)imm; break;
  case Op::SLTIU:  result.value = src1 < imm; break;
  case Op::XORI:   result.value = src1 ^ imm; break;
  case Op::ORI:    result.value = src1 | imm; break;
  case Op::ANDI:   result.value = src1 & imm; break;
  case Op::SLLI:   result.value = src1 << (imm & 31); break;
  case Op::SRLI:   result.value = src1 >> (imm & 31); break;
  case Op::SRAI:   result.value = (uint32_t)(signedSrc1 >> (imm & 31)); break;

  case Op::ADD:    result.value = src1 + src2; break;
  case Op::SUB:    result.value = src1 - src2; break;
  case Op::SLL:    result.value = src1 << (src2 & 31); break;
  case Op::SLT:    result.value = signedSrc1 < signedSrc2; break;
  case Op::SLTU:   result.value = src1 < src2; break;
  case Op::XOR:    result.value = src1 ^ src2; break;
  case Op::SRL:    result.value = src1 >> (src2 & 31); break;
  case Op::SRA:    result.value = (uint32_t)(signedSrc1 >> (src2 & 31)); break;
  case Op::OR:     result.value = src1 | src2; break;
  case Op::AND:    result.value = src1 & src2; break;

  case Op::MUL:    result.value = src1 * src2; break;
  case Op::MULH:   result.value = (uint32_t)(((int64_t)signedSrc1 * (int64_t)signedSrc2) >> 32); break;
  case Op::MULHSU: result.value = (uint32_t)(((int64_t)signedSrc1 * (int64_t)(uint64_t)src2) >> 32); break;
  case Op::MULHU:  result.value = (uint32_t)(((uint64_t)src1 * (uint64_t)src2) >> 32); break;

  // RISC-V M-extension defines explicit results for divide-by-zero and
  // INT32_MIN / -1 instead of trapping.
  case Op::DIV:
    if (src2 == 0) result.value = 0xFFFFFFFFu;
    else if (src1 == 0x80000000u && signedSrc2 == -1) result.value = src1;
    else result.value = (uint32_t)(signedSrc1 / signedSrc2);
    break;
  case Op::DIVU:
    result.value = (src2 == 0) ? 0xFFFFFFFFu : src1 / src2;
    break;
  case Op::REM:
    if (src2 == 0) result.value = src1;
    else if (src1 == 0x80000000u && signedSrc2 == -1) result.value = 0;
    else result.value = (uint32_t)(signedSrc1 % signedSrc2);
    break;
  case Op::REMU:
    result.value = (src2 == 0) ? src1 : src1 % src2;
    break;

  case Op::FENCE: case Op::ECALL: case Op::EBREAK: case Op::ILLEGAL:
    break;
  }

  if (isBranch(instr.op) && result.redirect)
    result.target = pc + imm;

  return result;
}

void dumpRegisters(const uint32_t regs[32], uint32_t pc) {
  fprintf(stderr, "--- registers ---\n");

  for (int i = 0; i < 32; i++)
    fprintf(stderr, "%4s=%08x%s", kRegName[i], regs[i], (i % 4 == 3) ? "\n" : "  ");

  fprintf(stderr, "pc  =%08x\n", pc);
}

std::string disasm(const Instr &instr) {
  char buf[64];
  const char *mnemonic = opName(instr.op);
  const char *rd = kRegName[instr.rd];
  const char *rs1 = kRegName[instr.rs1];
  const char *rs2 = kRegName[instr.rs2];

  if (instr.op == Op::LUI || instr.op == Op::AUIPC)
    snprintf(buf, sizeof buf, "%s %s,0x%x", mnemonic, rd, (uint32_t)instr.imm >> 12);
  else if (instr.op == Op::JAL)
    snprintf(buf, sizeof buf, "%s %s,%d", mnemonic, rd, instr.imm);
  else if (instr.op == Op::JALR || isLoad(instr.op))
    snprintf(buf, sizeof buf, "%s %s,%d(%s)", mnemonic, rd, instr.imm, rs1);
  else if (isBranch(instr.op))
    snprintf(buf, sizeof buf, "%s %s,%s,%d", mnemonic, rs1, rs2, instr.imm);
  else if (isStore(instr.op))
    snprintf(buf, sizeof buf, "%s %s,%d(%s)", mnemonic, rs2, instr.imm, rs1);
  else if (instr.op >= Op::ADDI && instr.op <= Op::SRAI)
    snprintf(buf, sizeof buf, "%s %s,%s,%d", mnemonic, rd, rs1, instr.imm);
  else if (instr.op == Op::ILLEGAL)
    snprintf(buf, sizeof buf, ".word 0x%08x", instr.raw);
  else if (isSystem(instr.op))
    snprintf(buf, sizeof buf, "%s", mnemonic);
  else
    snprintf(buf, sizeof buf, "%s %s,%s,%s", mnemonic, rd, rs1, rs2);

  return buf;
}
