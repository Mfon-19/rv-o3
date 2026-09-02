// Instruction decode: raw 32-bit words -> Instr records.
//
// One helper per immediate format; each diagram shows where the
// immediate's bits live in the encoding.

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
// | 31      | 30     25 | 24 20 | 19   15 | 14  12 | 11     8 | 7       | 6    0 |
// | imm[12] | imm[10:5] |  rs2  |   rs1   | funct3 | imm[4:1] | imm[11] | opcode |
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

namespace {

// Opcode groups indexed by funct3; ILLEGAL marks unused encodings
constexpr Op kBranch[8] = {Op::BEQ, Op::BNE, Op::ILLEGAL, Op::ILLEGAL,
                           Op::BLT, Op::BGE, Op::BLTU,    Op::BGEU};
constexpr Op kLoad[8] = {Op::LB,  Op::LH,  Op::LW,      Op::ILLEGAL,
                         Op::LBU, Op::LHU, Op::ILLEGAL, Op::ILLEGAL};
constexpr Op kStore[8] = {Op::SB,      Op::SH,      Op::SW,      Op::ILLEGAL,
                          Op::ILLEGAL, Op::ILLEGAL, Op::ILLEGAL, Op::ILLEGAL};
constexpr Op kOpImm[8] = {Op::ADDI, Op::SLLI, Op::SLTI, Op::SLTIU,
                          Op::XORI, Op::SRLI, Op::ORI,  Op::ANDI};
constexpr Op kOp[8] = {Op::ADD, Op::SLL, Op::SLT, Op::SLTU,
                       Op::XOR, Op::SRL, Op::OR,  Op::AND};
constexpr Op kOpAlt[8] = {Op::SUB,     Op::ILLEGAL, Op::ILLEGAL, Op::ILLEGAL,
                          Op::ILLEGAL, Op::SRA,     Op::ILLEGAL, Op::ILLEGAL};
constexpr Op kMulDiv[8] = {Op::MUL, Op::MULH, Op::MULHSU, Op::MULHU,
                           Op::DIV, Op::DIVU, Op::REM,    Op::REMU};

} // namespace

Instr decode(uint32_t w) {
  Instr I;
  I.raw = w;
  // rd, rs1 and rs2 sit in the same bits in every format (usesRs1 and
  // friends say whether a given op really reads them)
  I.rd = (w >> 7) & 31;
  I.rs1 = (w >> 15) & 31;
  I.rs2 = (w >> 20) & 31;
  const uint32_t funct3 = (w >> 12) & 7;
  const uint32_t funct7 = w >> 25;

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
  case 0x63:
    I.op = kBranch[funct3];
    I.imm = immB(w);
    break;
  case 0x03:
    I.op = kLoad[funct3];
    I.imm = immI(w);
    break;
  case 0x23:
    I.op = kStore[funct3];
    I.imm = immS(w);
    break;
  case 0x13:
    I.op = kOpImm[funct3];
    I.imm = immI(w);
    if (funct3 == 1 || funct3 == 5) { // shifts: shamt sits in the rs2 field
      if (funct3 == 5 && funct7 == 0x20)
        I.op = Op::SRAI;
      else if (funct7 != 0x00)
        I.op = Op::ILLEGAL;
      if (I.op != Op::ILLEGAL)
        I.imm = I.rs2;
    }
    break;
  case 0x33:
    if (funct7 == 0x00)
      I.op = kOp[funct3];
    else if (funct7 == 0x20)
      I.op = kOpAlt[funct3];
    else if (funct7 == 0x01)
      I.op = kMulDiv[funct3]; // M extension
    break;
  case 0x0F:
    I.op = Op::FENCE;
    break; // single hart: nothing to order
  case 0x73: // SYSTEM
    if (w == 0x00000073)
      I.op = Op::ECALL;
    if (w == 0x00100073)
      I.op = Op::EBREAK;
    break; // CSR instructions stay ILLEGAL
  }
  return I;
}
