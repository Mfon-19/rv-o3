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
ExecResult execute(const Instr &instr, uint32_t pc, uint32_t src1, uint32_t src2);
std::string disasm(const Instr &instr);
