// The functional reference model: the executable form of the ISA spec.
//
// A plain fetch-decode-execute-commit interpreter. One instruction at a
// time, in program order, each fully completed before the next begins.
// No pipeline, no forwarding, no speculation, no stalls; nothing here
// can be wrong about *when*, because there is no when. It answers only
// what each instruction does to architectural state.
//
// It shares decode() and execute() with the timing core, so it is not
// an independent oracle for those; the two can only diverge in what
// the core adds on top: operand routing, hazard handling, memory
// ordering, speculation, recovery, retirement. That is exactly what
// comparing their commit streams (core/commit.h) checks. The bench/
// host builds cover the ISA semantics from outside.
//
// The non-ISA policies are shared outright rather than mirrored: the
// syscall interface (sim/syscall.h) and the fatal-access checks
// (memory/memory.h) are the same code in both models.

#pragma once

#include <cstdint>

#include "core/commit.h"
#include "isa/isa.h"
#include "memory/memory.h"
#include "sim/config.h"

class RefModel {
public:
  explicit RefModel(const SimConfig &cfg);

  Memory mem;

  // Suppress all output (syscall prints and halt messages) while still
  // honoring exit. Set when the model runs as a silent shadow of the
  // core in a differential check, so side effects happen once
  bool quiet = false;

  // Execute one instruction and optionally describe it in *rec.
  // Returns false if the model had already halted
  bool step(CommitRecord *rec = nullptr);

  // Run until an exit syscall / ebreak / error, or the instruction
  // budget runs out; returns the exit code
  int run();

  void dumpRegs() const { dumpRegisters(regs, pc); }

  bool halted() const { return halted_; }
  int exitCode() const { return exitCode_; }
  uint64_t retired() const { return retired_; }
  uint32_t reg(int i) const { return regs[i]; }

private:
  // The architectural state, apart from mem above
  uint32_t regs[32];
  uint32_t pc = 0;

  bool halted_ = false;
  int exitCode_ = 0;
  uint64_t retired_ = 0;
  uint64_t maxInstrs; // reuses the -c budget, counted in instructions
};
