// Execute semantics: the pure, timing-free result of one instruction.
//
// Given a decoded instruction, its pc, and its two resolved source
// operands, execute() computes what the instruction *means*: the value
// it produces and any control transfer it causes. Where the operands
// come from (the register file, a forwarding network, a physical
// register file...) and how long it takes are the timing model's
// business, not this file's. The five-stage core's EX stage and the
// functional reference model both call this same function.

#pragma once

#include "isa/isa.h"

struct ExecResult {
  // ALU result / effective address (loads and stores) / link address (jumps)
  uint32_t value = 0;
  bool redirect = false; // taken branch or jump
  uint32_t target = 0;   // new pc, meaningful only when redirect is set
};

// FENCE, ECALL, EBREAK, and ILLEGAL pass through with value 0; the
// timing model handles them at commit
ExecResult execute(const Instr &I, uint32_t pc, uint32_t a, uint32_t b);
