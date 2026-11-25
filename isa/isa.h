// RV32IM instruction definitions, classification helpers, and timing-free
// decode/execute/disassembly interfaces shared by both the out-of-order core
// and the reference interpreter.

#pragma once

#include <cstdint>
#include <string>

#define OP_LIST(_)                                                          \
    _(LUI, "lui") _(AUIPC, "auipc")                                         \
    _(JAL, "jal") _(JALR, "jalr")                                           \
    _(BEQ, "beq") _(BNE, "bne") _(BLT, "blt") _(BGE, "bge")                 \
    _(BLTU, "bltu") _(BGEU, "bgeu")                                         \
    _(LB, "lb") _(LH, "lh") _(LW, "lw") _(LBU, "lbu") _(LHU, "lhu")         \
    _(SB, "sb") _(SH, "sh") _(SW, "sw")                                     \
    _(ADDI, "addi") _(SLTI, "slti") _(SLTIU, "sltiu") _(XORI, "xori")       \
    _(ORI, "ori") _(ANDI, "andi") _(SLLI, "slli") _(SRLI, "srli")           \
    _(SRAI, "srai")                                                         \
    _(ADD, "add") _(SUB, "sub") _(SLL, "sll") _(SLT, "slt")                 \
    _(SLTU, "sltu") _(XOR, "xor") _(SRL, "srl") _(SRA, "sra")               \
    _(OR, "or") _(AND, "and")                                               \
    _(MUL, "mul") _(MULH, "mulh") _(MULHSU, "mulhsu") _(MULHU, "mulhu")     \
    _(DIV, "div") _(DIVU, "divu") _(REM, "rem") _(REMU, "remu")             \
    _(FENCE, "fence") _(ECALL, "ecall") _(EBREAK, "ebreak")                 \
    _(ILLEGAL, ".illegal")

enum class Op : uint8_t {
#define X(enumName, mnemonic) enumName,
  OP_LIST(X)
#undef X
};

inline const char *opName(Op op) {
  switch (op) {
#define X(enumName, mnemonic) case Op::enumName: return mnemonic;
    OP_LIST(X)
#undef X
  }
  return "?unknown?";
}

struct Instr {
  Op op = Op::ILLEGAL;
  uint8_t rd = 0, rs1 = 0, rs2 = 0;
  int32_t imm = 0;
  uint32_t raw = 0;
};

struct ExecResult {
  uint32_t value = 0;
  bool redirect = false;
  uint32_t target = 0;
};

enum class FuKind : uint8_t { ALU, BRANCH, MUL, DIV, LSU, NONE };

inline bool isLoad(Op op) { return op >= Op::LB && op <= Op::LHU; }
inline bool isStore(Op op) { return op >= Op::SB && op <= Op::SW; }
inline bool isBranch(Op op) { return op >= Op::BEQ && op <= Op::BGEU; }
inline bool isSystem(Op op) { return op >= Op::FENCE; }

inline bool writesRd(Op op) {
  return !isBranch(op) && !isStore(op) && !isSystem(op);
}

inline bool usesRs1(Op op) {
  return op != Op::LUI && op != Op::AUIPC && op != Op::JAL && !isSystem(op);
}

inline bool usesRs2(Op op) {
  return isBranch(op) || isStore(op) || (op >= Op::ADD && op <= Op::REMU);
}

inline FuKind fuKindOf(Op op) {
  if (isLoad(op) || isStore(op)) return FuKind::LSU;
  if (isBranch(op) || op == Op::JAL || op == Op::JALR) return FuKind::BRANCH;
  if (op >= Op::MUL && op <= Op::MULHU) return FuKind::MUL;
  if (op >= Op::DIV && op <= Op::REMU) return FuKind::DIV;
  return isSystem(op) ? FuKind::NONE : FuKind::ALU;
}

inline uint32_t accessSize(Op op) {
  if (op == Op::LB || op == Op::LBU || op == Op::SB) return 1;
  if (op == Op::LH || op == Op::LHU || op == Op::SH) return 2;
  return 4;
}

// Sign- or zero-extend raw memory data according to the load width.
inline uint32_t extendLoad(Op op, uint32_t raw) {
  switch (op) {
  case Op::LB:  return (uint32_t)(int32_t)(int8_t)raw;
  case Op::LBU: return raw & 0xFF;
  case Op::LH:  return (uint32_t)(int32_t)(int16_t)raw;
  case Op::LHU: return raw & 0xFFFF;
  default:      return raw;
  }
}

extern const char *const kRegName[32];

void dumpRegisters(const uint32_t regs[32], uint32_t pc);
Instr decode(uint32_t word);
std::string disasm(const Instr &instr);

inline ExecResult execute(const Instr &instr, uint32_t pc, uint32_t src1, uint32_t src2) {
  ExecResult result;
  const int32_t signedSrc1 = (int32_t)src1;
  const int32_t signedSrc2 = (int32_t)src2;
  const uint32_t imm = (uint32_t)instr.imm;

  switch (instr.op) {
  case Op::LUI:    result.value = imm; break;
  case Op::AUIPC:  result.value = pc + imm; break;
  case Op::JAL:    result = {pc + 4, true, pc + imm}; break;
  case Op::JALR:   result = {pc + 4, true, (src1 + imm) & ~1u}; break;

  case Op::BEQ:    if (src1 == src2) { result.redirect = true; result.target = pc + imm; } break;
  case Op::BNE:    if (src1 != src2) { result.redirect = true; result.target = pc + imm; } break;
  case Op::BLT:    if (signedSrc1 < signedSrc2) { result.redirect = true; result.target = pc + imm; } break;
  case Op::BGE:    if (signedSrc1 >= signedSrc2) { result.redirect = true; result.target = pc + imm; } break;
  case Op::BLTU:   if (src1 < src2) { result.redirect = true; result.target = pc + imm; } break;
  case Op::BGEU:   if (src1 >= src2) { result.redirect = true; result.target = pc + imm; } break;

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

  return result;
}
