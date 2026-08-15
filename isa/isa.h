// ==============================================================
// ISA definitions: opcodes, decoded-instruction record, classification
// ==============================================================
//
// RV32I base integer ISA plus the M extension for mul/div, little-endian.
// FENCE is executed as a NOP, ECALL provides a tiny syscall interface so
// programs can print and exit, EBREAK stops the simulation.
//
// Everything in isa/ is timing-free: pure decode, pure execute semantics,
// and disassembly. Any timing model (the five-stage core today, other
// cores later) and the functional reference model all build on these
// same definitions, so their architectural behavior cannot diverge.

#pragma once

#include <cstdint>
#include <string>

#define OP_LIST(_)                                                          \
    /* U-type */                                                            \
    _(LUI, "lui") _(AUIPC, "auipc")                                         \
    /* jumps */                                                             \
    _(JAL, "jal") _(JALR, "jalr")                                           \
    /* conditional branches (B-type )*/                                     \
    _(BEQ, "beq") _(BNE, "bne") _(BLT, "blt") _(BGE, "bge")                 \
    _(BLTU, "bltu") _(BGEU, "bgeu")                                         \
    /* loads (I-type) */                                                    \
    _(LB, "lb") _(LH, "lh") _(LW, "lw") _(LBU, "lbu") _(LHU, "lhu")         \
    /* stores (S-type) */                                                   \
    _(SB, "sb") _(SH, "sh") _(SW, "sw")                                     \
    /* ALU with immediate (I-type) */                                       \
    _(ADDI, "addi") _(SLTI, "slti") _(SLTIU, "sltiu") _(XORI, "xori")       \
    _(ORI, "ori") _(ANDI, "andi") _(SLLI, "slli") _(SRLI, "srli")           \
    _(SRAI, "srai")                                                         \
    /* ALU register-register (R-type) */                                    \
    _(ADD, "add") _(SUB, "sub") _(SLL, "sll") _(SLT, "slt")                 \
    _(SLTU, "sltu") _(XOR, "xor") _(SRL, "srl") _(SRA, "sra")               \
    _(OR, "or") _(AND, "and")                                               \
    /* M extension (R-type, funct7 = 0000001) */                            \
    _(MUL, "mul") _(MULH, "mulh") _(MULHSU, "mulhsu") _(MULHU, "mulhu")     \
    _(DIV, "div") _(DIVU, "divu") _(REM, "rem") _(REMU, "remu")             \
    /* system */                                                            \
    _(FENCE, "fence") _(ECALL, "ecall") _(EBREAK, "ebreak")                 \
    /* a catch-all for anything we can't decode */                          \
    _(ILLEGAL, ".illegal")

enum class Op : uint8_t {
#define X(e, s) e,
  // Expands to all the instructions in uppercase
  OP_LIST(X)
#undef X
};

inline const char *opName(Op op) {
  switch (op) {
#define X(e, s)                                                                \
  case Op::e:                                                                  \
    return s;
    OP_LIST(X)
#undef X
  }
  return "?unknown?";
}

// A fully decoded instruction. We decode once and carry this record
// through whatever model is executing it
struct Instr {
  Op op = Op::ILLEGAL;
  uint8_t rd = 0, rs1 = 0, rs2 = 0; // register specifier fields
  int32_t imm = 0;                  // sign-extended immediate, if any
  uint32_t raw = 0;                 // original encoding, kept for messages
};

// Functions down here are for instruction classification,
// so called "control signals" in hardware
inline bool isLoad(Op op) { return op >= Op::LB && op <= Op::LHU; }

inline bool isStore(Op op) { return op >= Op::SB && op <= Op::SW; }

inline bool isBranch(Op op) { return op >= Op::BEQ && op <= Op::BGEU; }

// Does this instruction write a destination register in WB?
inline bool writesRd(Op op) {
  if (isBranch(op) || isStore(op))
    return false;
  switch (op) {
  case Op::FENCE:
  case Op::ECALL:
  case Op::EBREAK:
  case Op::ILLEGAL:
    return false;
  default:
    // U-type, jumps, loads, all ALU and M-extension ops
    return true;
  }
}

// Does this encoding's rs1 field name a real source register? For
// U/J-type instructions those bits are immediate bits, so we must not
// treat them as a register use when checking hazards
inline bool usesRs1(Op op) {
  switch (op) {
  case Op::LUI:
  case Op::AUIPC:
  case Op::JAL:
  case Op::FENCE:
  case Op::ECALL:
  case Op::EBREAK:
  case Op::ILLEGAL:
    return false;
  default:
    return true;
  }
}

inline bool usesRs2(Op op) {
  // all R-type including M extension
  return isBranch(op) || isStore(op) || (op >= Op::ADD && op <= Op::REMU);
}

// Which functional unit executes each op. System ops (fence,
// ecall, ebreak, illegal) need no unit: they serialize the machine and
// take effect at retirement
enum class FuKind : uint8_t { ALU, BRANCH, MUL, DIV, LSU, NONE };

inline FuKind fuKindOf(Op op) {
  if (isLoad(op) || isStore(op))
    return FuKind::LSU;
  if (isBranch(op) || op == Op::JAL || op == Op::JALR)
    return FuKind::BRANCH;
  if (op >= Op::MUL && op <= Op::MULHU)
    return FuKind::MUL;
  if (op >= Op::DIV && op <= Op::REMU)
    return FuKind::DIV;
  switch (op) {
  case Op::FENCE:
  case Op::ECALL:
  case Op::EBREAK:
  case Op::ILLEGAL:
    return FuKind::NONE;
  default:
    return FuKind::ALU; // U-type and all ALU ops
  }
}

// Memory access width in bytes
inline uint32_t accessSize(Op op) {
  switch (op) {
  case Op::LB:
  case Op::LBU:
  case Op::SB:
    return 1;
  case Op::LH:
  case Op::LHU:
  case Op::SH:
    return 2;
  default:
    return 4;
  }
}

// Sub-word loads sign- or zero-extended into 32 bits as per the ISA
inline uint32_t extendLoad(Op op, uint32_t raw) {
  switch (op) {
  case Op::LB:
    return (uint32_t)(int32_t)(int8_t)raw;
  case Op::LBU:
    return raw & 0xFF;
  case Op::LH:
    return (uint32_t)(int32_t)(int16_t)raw;
  case Op::LHU:
    return raw & 0xFFFF;
  default:
    return raw;
  }
}

// ABI register names, indexed by register number
extern const char *const kRegName[32];

// Decode one 32-bit instruction word (decode.cpp)
Instr decode(uint32_t w);

// Disassemble a decoded instruction, used for traces and error
// messages (disasm.cpp)
std::string disasm(const Instr &I);
