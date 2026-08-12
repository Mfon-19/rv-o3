// Instruction decode: raw 32-bit words -> Instr records.
//
// The immediate helpers extract the immediate fields from each
// instruction type.

#include "isa/isa.h"

// I-type format:
// +--------------------------+---------+--------+-------+--------+
// | 31                    20 | 19   15 | 14  12 | 11  7 | 6    0 |
// |        imm[11:0]         |   rs1   | funct3 |  rd   | opcode |
// +--------------------------+---------+--------+-------+--------+
// For non-shift I-type instructions, the immediate is in bits [31:20] and
// is sign-extended from bit 31. RV32 shift-immediate instructions instead
// use bits [24:20] as shamt; bits [31:25] are imm[11:5] and must match
// the funct7-like pattern for SLLI/SRLI/SRAI.
// Shift-immediate sub-format:
// +----------+-------+---------+--------+-------+--------+
// | 31    25 | 24 20 | 19   15 | 14  12 | 11  7 | 6    0 |
// | imm[11:5]| shamt |   rs1   | funct3 |  rd   | opcode |
// +----------+-------+---------+--------+-------+--------+
static int32_t immI(uint32_t w) { return (int32_t)w >> 20; }

// S-type format:
// +-----------------+-------+---------+--------+----------+--------+
// | 31           25 | 24 20 | 19   15 | 14  12 | 11     7 | 6    0 |
// |    imm[11:5]    |  rs2  |   rs1   | funct3 | imm[4:0] | opcode |
// +-----------------+-------+---------+--------+----------+--------+
// Immediate is sign-extended from bit 31 and is split into:
// - imm[11:5] in bits [31:25]
// - imm[4:0]  in bits [11:7]
static int32_t immS(uint32_t w) {
  return ((int32_t)(w & 0xFE000000u) >> 20) | (int32_t)((w >> 7) & 0x1F);
}

// B-type format:
// +---------+-----------+-------+---------+--------+----------+---------+--------+
// | 31      | 30     25 | 24 20 | 19   15 | 14  12 | 11     8 | 7       | 6 0 |
// | imm[12] | imm[10:5] |  rs2  |   rs1   | funct3 | imm[4:1] | imm[11] |
// opcode |
// +---------+-----------+-------+---------+--------+----------+---------+--------+
// Immediate represents a signed branch offset, sign-extended from bit 31,
// and is split into:
// - imm[12]   in bit 31
// - imm[11]   in bit 7
// - imm[10:5] in bits [30:25]
// - imm[4:1]  in bits [11:8]
// - imm[0]    is implicitly 0
static int32_t immB(uint32_t w) {
  return ((int32_t)(w & 0x80000000u) >> 19) |
         (int32_t)((w & 0x00000080u) << 4) | (int32_t)((w >> 20) & 0x7E0) |
         (int32_t)((w >> 7) & 0x1E);
}

// U-type format:
// +--------------------------------------------+-------+--------+
// | 31                                      12 | 11  7 | 6    0 |
// |                 imm[31:12]                 |  rd   | opcode |
// +--------------------------------------------+-------+--------+
// Immediate bits [31:12] are in bits [31:12]; bits [11:0] are zero.
static int32_t immU(uint32_t w) { return (int32_t)(w & 0xFFFFF000u); }

// J-type format:
// +---------+-----------+---------+------------+-------+--------+
// | 31      | 30     21 | 20      | 19      12 | 11  7 | 6    0 |
// | imm[20] | imm[10:1] | imm[11] | imm[19:12] |  rd   | opcode |
// +---------+-----------+---------+------------+-------+--------+
// Immediate represents a signed jump offset, sign-extended from bit 31,
// and is split into:
// - imm[20]    in bit 31
// - imm[19:12] in bits [19:12]
// - imm[11]    in bit 20
// - imm[10:1]  in bits [30:21]
// - imm[0]     is implicitly 0
static int32_t immJ(uint32_t w) {
  return ((int32_t)(w & 0x80000000u) >> 11) | (int32_t)(w & 0x000FF000u) |
         (int32_t)((w >> 9) & 0x800) | (int32_t)((w >> 20) & 0x7FE);
}

Instr decode(uint32_t w) {
  Instr I;
  I.raw = w;
  // rd, rs1 and rs2 are in the same spots for all instructions
  I.rd = (w >> 7) & 31;
  I.rs1 = (w >> 15) & 31;
  I.rs2 = (w >> 20) & 31;
  const uint32_t funct3 = (w >> 12) & 7;
  const uint32_t funct7 = w >> 25;

  // switch on the opcode, lower 7 bits
  switch (w & 0x7F) {
  case 0x37:
    I.op = Op::LUI;
    I.imm = immU(w);
    break;
  case 0x17:
    I.op = Op::AUIPC;
    I.imm = immU(w);
    break;
  case 0x6F:
    I.op = Op::JAL;
    I.imm = immJ(w);
    break;
  case 0x67:
    if (funct3 == 0) {
      I.op = Op::JALR;
      I.imm = immI(w);
    }
    break;
  case 0x63: // conditional branches
    I.imm = immB(w);
    switch (funct3) {
    case 0:
      I.op = Op::BEQ;
      break;
    case 1:
      I.op = Op::BNE;
      break;
    case 4:
      I.op = Op::BLT;
      break;
    case 5:
      I.op = Op::BGE;
      break;
    case 6:
      I.op = Op::BLTU;
      break;
    case 7:
      I.op = Op::BGEU;
      break;
    }
    break;
  case 0x03: // loads
    I.imm = immI(w);
    switch (funct3) {
    case 0:
      I.op = Op::LB;
      break;
    case 1:
      I.op = Op::LH;
      break;
    case 2:
      I.op = Op::LW;
      break;
    case 4:
      I.op = Op::LBU;
      break;
    case 5:
      I.op = Op::LHU;
      break;
    }
    break;
  case 0x23: // stores
    I.imm = immS(w);
    switch (funct3) {
    case 0:
      I.op = Op::SB;
      break;
    case 1:
      I.op = Op::SH;
      break;
    case 2:
      I.op = Op::SW;
      break;
    }
    break;
  case 0x13: // ALU with immediate
    I.imm = immI(w);
    switch (funct3) {
    case 0:
      I.op = Op::ADDI;
      break;
    case 2:
      I.op = Op::SLTI;
      break;
    case 3:
      I.op = Op::SLTIU;
      break;
    case 4:
      I.op = Op::XORI;
      break;
    case 6:
      I.op = Op::ORI;
      break;
    case 7:
      I.op = Op::ANDI;
      break;
    case 1: // shifts encode shamt in the rs2 field
      if (funct7 == 0x00) {
        I.op = Op::SLLI;
        I.imm = I.rs2;
      }
      break;
    case 5:
      if (funct7 == 0x00) {
        I.op = Op::SRLI;
        I.imm = I.rs2;
      }
      if (funct7 == 0x20) {
        I.op = Op::SRAI;
        I.imm = I.rs2;
      }
      break;
    }
    break;
  case 0x33: // ALU register-register
    if (funct7 == 0x00) {
      switch (funct3) {
      case 0:
        I.op = Op::ADD;
        break;
      case 1:
        I.op = Op::SLL;
        break;
      case 2:
        I.op = Op::SLT;
        break;
      case 3:
        I.op = Op::SLTU;
        break;
      case 4:
        I.op = Op::XOR;
        break;
      case 5:
        I.op = Op::SRL;
        break;
      case 6:
        I.op = Op::OR;
        break;
      case 7:
        I.op = Op::AND;
        break;
      }
    } else if (funct7 == 0x20) {
      if (funct3 == 0)
        I.op = Op::SUB;
      if (funct3 == 5)
        I.op = Op::SRA;
    } else if (funct7 == 0x01) { // M extension
      switch (funct3) {
      case 0:
        I.op = Op::MUL;
        break;
      case 1:
        I.op = Op::MULH;
        break;
      case 2:
        I.op = Op::MULHSU;
        break;
      case 3:
        I.op = Op::MULHU;
        break;
      case 4:
        I.op = Op::DIV;
        break;
      case 5:
        I.op = Op::DIVU;
        break;
      case 6:
        I.op = Op::REM;
        break;
      case 7:
        I.op = Op::REMU;
        break;
      }
    }
    break;
  case 0x0F:
    I.op = Op::FENCE;
    break;   // NOP for us: single hart
  case 0x73: // SYSTEM
    if (w == 0x00000073)
      I.op = Op::ECALL;
    if (w == 0x00100073)
      I.op = Op::EBREAK;
    break; // CSR instructions: ILLEGAL
  }
  return I;
}
