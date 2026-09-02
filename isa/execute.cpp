#include "isa/execute.h"

ExecResult execute(const Instr &I, uint32_t pc, uint32_t a, uint32_t b) {
  ExecResult out;
  const int32_t sa = (int32_t)a, sb = (int32_t)b; // signed values
  const uint32_t imm = (uint32_t)I.imm;

  switch (I.op) {
  // upper immediate
  case Op::LUI:
    out.value = imm;
    break;
  case Op::AUIPC:
    out.value = pc + imm;
    break;

  // jumps: link address is pc + 4, always redirect
  case Op::JAL:
    out.value = pc + 4;
    out.redirect = true;
    out.target = pc + imm;
    break;
  case Op::JALR:
    out.value = pc + 4;
    out.redirect = true;
    out.target = (a + imm) & ~1u; // ISA clears bit 0
    break;

  // conditional branches: redirect only if taken (target below)
  case Op::BEQ:
    out.redirect = a == b;
    break;
  case Op::BNE:
    out.redirect = a != b;
    break;
  case Op::BLT:
    out.redirect = sa < sb;
    break;
  case Op::BGE:
    out.redirect = sa >= sb;
    break;
  case Op::BLTU:
    out.redirect = a < b;
    break;
  case Op::BGEU:
    out.redirect = a >= b;
    break;

  // memory: execute only computes the effective address
  case Op::LB:
  case Op::LH:
  case Op::LW:
  case Op::LBU:
  case Op::LHU:
  case Op::SB:
  case Op::SH:
  case Op::SW:
    out.value = a + imm;
    break;

  // ALU, immediate
  case Op::ADDI:
    out.value = a + imm;
    break;
  case Op::SLTI:
    out.value = sa < (int32_t)imm;
    break;
  case Op::SLTIU:
    out.value = a < imm;
    break;
  case Op::XORI:
    out.value = a ^ imm;
    break;
  case Op::ORI:
    out.value = a | imm;
    break;
  case Op::ANDI:
    out.value = a & imm;
    break;
  case Op::SLLI:
    out.value = a << (imm & 31);
    break;
  case Op::SRLI:
    out.value = a >> (imm & 31);
    break;
  case Op::SRAI:
    out.value = (uint32_t)(sa >> (imm & 31));
    break;

  // ALU, register
  case Op::ADD:
    out.value = a + b;
    break;
  case Op::SUB:
    out.value = a - b;
    break;
  case Op::SLL:
    out.value = a << (b & 31);
    break;
  case Op::SLT:
    out.value = sa < sb;
    break;
  case Op::SLTU:
    out.value = a < b;
    break;
  case Op::XOR:
    out.value = a ^ b;
    break;
  case Op::SRL:
    out.value = a >> (b & 31);
    break;
  case Op::SRA:
    out.value = (uint32_t)(sa >> (b & 31));
    break;
  case Op::OR:
    out.value = a | b;
    break;
  case Op::AND:
    out.value = a & b;
    break;

  // M extension. RISC-V defines results for divide-by-zero and signed
  // overflow instead of trapping; C does not, so they are spelled out
  case Op::MUL:
    out.value = a * b;
    break;
  case Op::MULH:
    out.value = (uint32_t)(((int64_t)sa * (int64_t)sb) >> 32);
    break;
  case Op::MULHSU:
    out.value = (uint32_t)(((int64_t)sa * (int64_t)(uint64_t)b) >> 32);
    break;
  case Op::MULHU:
    out.value = (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32);
    break;
  case Op::DIV:
    if (b == 0)
      out.value = 0xFFFFFFFFu; // -1
    else if (a == 0x80000000u && sb == -1)
      out.value = a; // overflow
    else
      out.value = (uint32_t)(sa / sb);
    break;
  case Op::DIVU:
    out.value = (b == 0) ? 0xFFFFFFFFu : a / b;
    break;
  case Op::REM:
    if (b == 0)
      out.value = a;
    else if (a == 0x80000000u && sb == -1)
      out.value = 0;
    else
      out.value = (uint32_t)(sa % sb);
    break;
  case Op::REMU:
    out.value = (b == 0) ? a : a % b;
    break;

  // no execute work; these take effect at commit or not at all
  case Op::FENCE:
  case Op::ECALL:
  case Op::EBREAK:
  case Op::ILLEGAL:
    break;
  }

  // Branch targets are pc-relative (jumps formed theirs above)
  if (isBranch(I.op) && out.redirect)
    out.target = pc + imm;

  return out;
}
